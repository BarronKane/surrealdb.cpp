#pragma once

/// @file connection.hpp
/// The database handle, and the operations that hang off it.
///
/// One owned `sr_surreal_t*`, move-only, closed by its destructor. Every
/// fallible call returns `result<T>` and nothing throws -- the library targets
/// `-fno-exceptions` builds.
///
/// **Poisoning.** The C library documents that once any operation returns
/// `SR_FATAL` the handle is poisoned and must not be used again; since
/// surrealdb.c 0.2.1 it enforces that itself and reports `SR_FATAL` on every
/// subsequent call rather than aborting the process. This class latches the
/// same state so `poisoned()` can be asked without making another call, and so
/// a caller who ignores one return value is not silently issuing more work
/// against a dead handle.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "detail/invoke.hpp"
#include "detail/owned.hpp"
#include "error.hpp"
#include "array.hpp"
#include "object.hpp"
#include "options.hpp"
#include "results.hpp"
#include "dsl/arena.hpp"
#include "stream.hpp"
#include "value.hpp"

#include <memory>
#include <string>
#include <utility>

namespace surrealdb {

/// Authentication level for signin/signup.
enum class scope : int {
    root      = SR_SCOPE_ROOT,
    ns        = SR_SCOPE_NAMESPACE,
    database  = SR_SCOPE_DATABASE,
    record    = SR_SCOPE_RECORD,
};

/// Username and password. Borrowed: both must outlive the call.
struct credentials {
    const char* username{nullptr};
    const char* password{nullptr};
};

/// Namespace/database/access-method triple, required for record-level auth.
struct access {
    const char* ns{nullptr};
    const char* database{nullptr};
    const char* method{nullptr};
};

class connection;

/// A transaction that cancels itself unless committed.
///
/// This is the one place where scope really is the semantic: an early return,
/// or any path that forgets to commit, cancels rather than leaving the
/// transaction open. Move-only, and inert once committed or cancelled.
class transaction {
public:
    transaction(transaction&& other) noexcept
        : db_(other.db_), live_(other.live_) {
        other.db_ = nullptr;
        other.live_ = false;
    }
    transaction& operator=(transaction&&) = delete;
    transaction(const transaction&) = delete;
    transaction& operator=(const transaction&) = delete;

    ~transaction() { (void)cancel(); }

    [[nodiscard]] result<void> commit() noexcept;
    [[nodiscard]] result<void> cancel() noexcept;

    [[nodiscard]] bool active() const noexcept { return live_; }

private:
    friend class connection;
    explicit transaction(const sr_surreal_t* db) noexcept : db_(db), live_(true) {}

    const sr_surreal_t* db_;
    bool live_;
};

/// A connection to SurrealDB.
class connection {
public:
    connection() noexcept = default;

    connection(connection&&) noexcept = default;
    connection& operator=(connection&&) noexcept = default;
    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;

    /// Connect with the server defaults.
    ///
    /// `"mem://"` in-memory, `"surrealkv://file.skv"` on disk,
    /// `"ws://host:8000"` remote.
    [[nodiscard]] static result<connection> connect(const char* endpoint) {
        sr_surreal_t* raw = nullptr;
        return detail::invoke<connection>(
            [&](sr_string_t* e) { return ::sr_connect(e, &raw, endpoint); },
            [&](int) { return connection(raw); });
    }

    /// Connect with explicit options.
    [[nodiscard]] static result<connection> connect(const char* endpoint,
                                                    const options& opts) {
        // The C view borrows from `view`, which therefore has to outlive the
        // call -- hence a named local rather than a temporary.
        options::c_view view = opts.to_c();
        sr_surreal_t* raw = nullptr;
        return detail::invoke<connection>(
            [&](sr_string_t* e) {
                return ::sr_connect_with_options(e, &raw, endpoint, view.opts);
            },
            [&](int) { return connection(raw); });
    }

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(db_); }
    [[nodiscard]] const sr_surreal_t* raw() const noexcept { return db_.get(); }

    /// True once any call has reported `SR_FATAL`. The handle is unusable.
    [[nodiscard]] bool poisoned() const noexcept { return poisoned_; }

    /// Close early. The destructor does this anyway.
    void disconnect() noexcept { db_.reset(); }

    // -- session ------------------------------------------------------------

    [[nodiscard]] result<void> use_ns(const char* ns) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_use_ns(raw(), e, ns);
        }));
    }

    [[nodiscard]] result<void> use_db(const char* database) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_use_db(raw(), e, database);
        }));
    }

    /// Both at once, short-circuiting if the namespace fails.
    [[nodiscard]] result<void> use(const char* ns, const char* database) noexcept {
        auto r = use_ns(ns);
        if (!r) return r;
        return use_db(database);
    }

    // -- introspection ------------------------------------------------------

    [[nodiscard]] result<owned_string> version() noexcept {
        sr_string_t out = nullptr;
        return track(detail::invoke<owned_string>(
            [&](sr_string_t* e) { return ::sr_version(raw(), e, &out); },
            [&](int) { return owned_string(out); }));
    }

    [[nodiscard]] result<void> health() noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_health(raw(), e);
        }));
    }

    // -- authentication -----------------------------------------------------

    /// Sign in and return the token.
    ///
    /// `params` carries whatever else the access method's SIGNIN query reads.
    /// Record-level auth usually needs it: a SIGNIN clause may select on any
    /// field, so a user/password pair is the common case rather than the shape.
    /// The root, namespace and database levels authenticate on the credentials
    /// alone and ignore it.
    [[nodiscard]] result<owned_string> signin(scope level, credentials creds,
                                              const access* details = nullptr,
                                              const object_builder* params = nullptr) noexcept {
        return auth_call(&::sr_signin, level, creds, details, params);
    }

    /// Sign in at record level with the fields the SIGNIN query reads.
    ///
    /// The spelling the call usually wants: an access method plus a bag of
    /// fields, with no separate credentials.
    [[nodiscard]] result<owned_string> signin(const access& details,
                                              const object_builder& params) noexcept {
        return auth_call(&::sr_signin, scope::record, credentials{}, &details, &params);
    }

    /// Sign up and return the token.
    ///
    /// Almost always record level, and almost always needs `params`: a SIGNUP
    /// query decides what the new record contains, so every field it reads has
    /// to arrive here.
    [[nodiscard]] result<owned_string> signup(scope level, credentials creds,
                                              const access* details = nullptr,
                                              const object_builder* params = nullptr) noexcept {
        return auth_call(&::sr_signup, level, creds, details, params);
    }

    /// Sign up at record level with the fields the SIGNUP query reads.
    [[nodiscard]] result<owned_string> signup(const access& details,
                                              const object_builder& params) noexcept {
        return auth_call(&::sr_signup, scope::record, credentials{}, &details, &params);
    }

    [[nodiscard]] result<void> authenticate(const char* token) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_authenticate(raw(), e, token);
        }));
    }

    [[nodiscard]] result<void> invalidate() noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_invalidate(raw(), e);
        }));
    }

    // -- data ---------------------------------------------------------------

    [[nodiscard]] result<owned_values> select(const char* resource) noexcept {
        return values_call(&::sr_select, resource);
    }

    [[nodiscard]] result<owned_values> remove(const char* resource) noexcept {
        return values_call(&::sr_delete, resource);
    }

    /// Create a record. The created record is returned; pass nothing to
    /// discard it, which avoids the server building one.
    [[nodiscard]] result<owned_object> create(const char* resource,
                                              const object_builder& content) noexcept {
        sr_object_t made{};
        return track(detail::invoke<owned_object>(
            [&](sr_string_t* e) {
                return ::sr_create(raw(), e, &made, resource, content.raw());
            },
            [&](int) { return owned_object(made); }));
    }

    [[nodiscard]] result<void> create_discarding(const char* resource,
                                                 const object_builder& content) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_create(raw(), e, nullptr, resource, content.raw());
        }));
    }

    [[nodiscard]] result<owned_values> update(const char* resource,
                                              const object_builder& content) noexcept {
        return content_call(&::sr_update, resource, content);
    }
    [[nodiscard]] result<owned_values> upsert(const char* resource,
                                              const object_builder& content) noexcept {
        return content_call(&::sr_upsert, resource, content);
    }
    [[nodiscard]] result<owned_values> merge(const char* resource,
                                             const object_builder& content) noexcept {
        return content_call(&::sr_merge, resource, content);
    }
    [[nodiscard]] result<owned_values> insert(const char* resource,
                                              const object_builder& content) noexcept {
        return content_call(&::sr_insert, resource, content);
    }

    [[nodiscard]] result<owned_values> relate(const char* from, const char* relation,
                                              const char* to,
                                              const object_builder& content) noexcept {
        sr_value_t* out = nullptr;
        return track(detail::invoke<owned_values>(
            [&](sr_string_t* e) {
                return ::sr_relate(raw(), e, &out, from, relation, to, content.raw());
            },
            [&](int n) { return owned_values(out, n); }));
    }

    /// Insert a graph relation, given the edge record whole.
    ///
    /// `relate()` names the two ends and the edge table separately; this takes
    /// a content object that already carries `in` and `out`, which is the
    /// shape you get back from a query.
    [[nodiscard]] result<owned_values> insert_relation(
        const char* table, const object_builder& content) noexcept {
        return content_call(&::sr_insert_relation, table, content);
    }

    /// Call a database function -- `fn::` definitions and built-ins alike.
    ///
    /// Named `call` rather than `run` because `run()` on this class already
    /// executes a built query. A scalar result comes back as a one-element
    /// array.
    [[nodiscard]] result<owned_values> call(const char* function_name,
                                            const array_builder& args) noexcept {
        sr_value_t* out = nullptr;
        return track(detail::invoke<owned_values>(
            [&](sr_string_t* e) {
                return ::sr_run(raw(), e, &out, function_name, args.raw());
            },
            [&](int n) { return owned_values(out, n); }));
    }

    /// Call a database function with no arguments.
    [[nodiscard]] result<owned_values> call(const char* function_name) noexcept {
        sr_value_t* out = nullptr;
        return track(detail::invoke<owned_values>(
            [&](sr_string_t* e) {
                return ::sr_run(raw(), e, &out, function_name, nullptr);
            },
            [&](int n) { return owned_values(out, n); }));
    }

    // -- JSON Patch ----------------------------------------------------------
    //
    // `path` is a JSON Pointer (RFC 6901): "/name", "/tags/0", "" for the
    // document itself. These edit one field without sending the whole record,
    // which `merge` cannot do for array elements.
    //
    // SurrealDB is more forgiving than RFC 6902 here, and the difference will
    // bite anyone who ports a patch document from elsewhere: a missing path is
    // not an error. `add` and `replace` create whatever intermediate objects
    // the path names -- `patch_replace(r, "/not/a/path", v)` succeeds and
    // leaves `{not:{a:{path:v}}}` behind -- and `remove` on a path that is not
    // there is a silent no-op. If you need strict behaviour, read the record
    // first; these calls will not tell you.

    /// Add a value at `path`. On an array index this inserts; on an existing
    /// object key it replaces. Missing parents are created.
    [[nodiscard]] result<owned_values> patch_add(const char* resource,
                                                 const char* path, value v) noexcept {
        return patch_call(&::sr_patch_add, resource, path, v);
    }

    /// Replace the value at `path`. Missing parents are created rather than
    /// rejected; see the note above.
    [[nodiscard]] result<owned_values> patch_replace(const char* resource,
                                                     const char* path, value v) noexcept {
        return patch_call(&::sr_patch_replace, resource, path, v);
    }

    /// Remove whatever is at `path`. A no-op if nothing is there.
    [[nodiscard]] result<owned_values> patch_remove(const char* resource,
                                                    const char* path) noexcept {
        sr_value_t* out = nullptr;
        return track(detail::invoke<owned_values>(
            [&](sr_string_t* e) {
                return ::sr_patch_remove(raw(), e, &out, resource, path);
            },
            [&](int n) { return owned_values(out, n); }));
    }

    /// Run a SurrealQL query.
    ///
    /// One entry per statement. A statement that fails at runtime does not
    /// fail this call -- see `query_results` and `statement_result`, and reach
    /// for `single()` when there is only one statement.
    [[nodiscard]] result<query_results> query(
        const char* surql, const object_builder* vars = nullptr) noexcept {
        sr_arr_res_t* out = nullptr;
        return track(detail::invoke<query_results>(
            [&](sr_string_t* e) {
                return ::sr_query(raw(), e, &out, surql, vars ? vars->raw() : nullptr);
            },
            [&](int n) { return query_results(owned_arr_results(out, n)); }));
    }

    // -- live queries -------------------------------------------------------

    /// Open a live query on a resource.
    ///
    /// The returned stream blocks on iteration and must be destroyed before
    /// this connection -- it borrows the connection's runtime. The stream
    /// carries a liveness token so that getting the order wrong is detected
    /// rather than silently running a kill against a dead runtime; see
    /// stream.hpp.
    [[nodiscard]] result<stream> live(const char* resource) noexcept {
        sr_stream_t* out = nullptr;
        return track(detail::invoke<stream>(
            [&](sr_string_t* e) { return ::sr_select_live(raw(), e, &out, resource); },
            [&](int) { return stream(out, std::weak_ptr<const void>(alive_)); }));
    }

    /// Kill a live query by its id, as reported by `notification::query_id`.
    ///
    /// **This strands any `stream` open on that query.** Delivery stops, but
    /// the stream stays open forever: it goes quiet and never reports its end,
    /// because a killed live query cannot be observed to end on this path. That
    /// is an upstream core defect rather than anything either library can work
    /// around, and `REMOVE TABLE` strands a stream the same way, so it is a
    /// property of the path and not of this call.
    ///
    /// To retire a live query you hold a `stream` for, call `stream::close()`
    /// instead -- it stops the query by a route the defect does not touch and
    /// releases the stream in the same step. Reach for `kill()` only for a
    /// query registered some other way, such as a bare `LIVE SELECT` run
    /// through `query()`, where there is no stream to strand.
    [[nodiscard]] result<void> kill(const char* query_id) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_kill(raw(), e, query_id);
        }));
    }

    /// Run a query built by the DSL.
    ///
    /// The statement and its bind variables travel together, so a value can
    /// never be mistaken for syntax.
    [[nodiscard]] result<query_results> run(const dsl::built_query& q) noexcept {
        sr_arr_res_t* out = nullptr;
        return track(detail::invoke<query_results>(
            [&](sr_string_t* e) {
                return ::sr_query(raw(), e, &out, q.c_str(), q.vars().raw());
            },
            [&](int n) { return query_results(owned_arr_results(out, n)); }));
    }

    // -- variables ----------------------------------------------------------

    [[nodiscard]] result<void> set(const char* key, value v) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_set(raw(), e, key, v.raw());
        }));
    }

    [[nodiscard]] result<void> unset(const char* key) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_unset(raw(), e, key);
        }));
    }

    // -- transactions -------------------------------------------------------

    /// Begin a transaction. It cancels on scope exit unless committed.
    [[nodiscard]] result<transaction> begin() noexcept {
        return track(detail::invoke<transaction>(
            [&](sr_string_t* e) { return ::sr_begin(raw(), e); },
            [&](int) { return transaction(raw()); }));
    }

    // -- import / export ----------------------------------------------------

    /// Export to a file. Note SurrealDB 3.x requires an imported file to begin
    /// with `OPTION IMPORT;`.
    [[nodiscard]] result<void> export_to(const char* path) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_export(raw(), e, path);
        }));
    }

    [[nodiscard]] result<void> import_from(const char* path) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_import(raw(), e, path);
        }));
    }

private:
    explicit connection(sr_surreal_t* raw) noexcept : db_(raw) {}

    /// Latch the poison flag from any result that reports SR_FATAL.
    template <class T>
    result<T> track(result<T> r) noexcept {
        if (!r.has_value() && r.error().is_fatal()) poisoned_ = true;
        return r;
    }

    using values_fn = int (*)(const sr_surreal_t*, sr_string_t*, sr_value_t**, const char*);
    result<owned_values> values_call(values_fn fn, const char* resource) noexcept {
        sr_value_t* out = nullptr;
        return track(detail::invoke<owned_values>(
            [&](sr_string_t* e) { return fn(raw(), e, &out, resource); },
            [&](int n) { return owned_values(out, n); }));
    }

    using content_fn = int (*)(const sr_surreal_t*, sr_string_t*, sr_value_t**,
                               const char*, const sr_object_t*);
    using patch_fn = int (*)(const sr_surreal_t*, sr_string_t*, sr_value_t**,
                             const char*, const char*, const sr_value_t*);

    result<owned_values> patch_call(patch_fn fn, const char* resource,
                                    const char* path, value v) noexcept {
        sr_value_t* out = nullptr;
        return track(detail::invoke<owned_values>(
            [&](sr_string_t* e) { return fn(raw(), e, &out, resource, path, v.raw()); },
            [&](int n) { return owned_values(out, n); }));
    }

    result<owned_values> content_call(content_fn fn, const char* resource,
                                      const object_builder& content) noexcept {
        sr_value_t* out = nullptr;
        return track(detail::invoke<owned_values>(
            [&](sr_string_t* e) { return fn(raw(), e, &out, resource, content.raw()); },
            [&](int n) { return owned_values(out, n); }));
    }

    using auth_fn = int (*)(const sr_surreal_t*, sr_string_t*, sr_string_t*,
                            const sr_credentials_scope*, const sr_credentials*,
                            const sr_credentials_access*, const sr_object_t*);
    result<owned_string> auth_call(auth_fn fn, scope level, credentials creds,
                                   const access* details,
                                   const object_builder* params) noexcept {
        const sr_credentials_scope c_scope = static_cast<sr_credentials_scope>(level);
        // sr_string_t is char*, so the C structs take non-const pointers even
        // though nothing is written through them.
        sr_credentials c_creds{const_cast<char*>(creds.username),
                               const_cast<char*>(creds.password)};
        sr_credentials_access c_access{};
        if (details) {
            c_access.namespace_ = const_cast<char*>(details->ns);
            c_access.database = const_cast<char*>(details->database);
            c_access.access = const_cast<char*>(details->method);
        }
        sr_string_t token = nullptr;
        return track(detail::invoke<owned_string>(
            [&](sr_string_t* e) {
                return fn(raw(), e, &token, &c_scope, &c_creds,
                          details ? &c_access : nullptr,
                          params ? params->raw() : nullptr);
            },
            [&](int) { return owned_string(token); }));
    }

    owned_connection db_;

    /// Liveness token handed to streams as a weak_ptr. It dies with this
    /// object, which is how a stream can tell that its connection -- and the
    /// runtime it borrows -- is gone.
    std::shared_ptr<const void> alive_{std::make_shared<const char>('\0')};

    bool poisoned_{false};
};

// ---------------------------------------------------------------------------

inline result<void> transaction::commit() noexcept {
    if (!live_) return ok();
    live_ = false;
    return detail::invoke([&](sr_string_t* e) { return ::sr_commit(db_, e); });
}

inline result<void> transaction::cancel() noexcept {
    if (!live_) return ok();
    live_ = false;
    return detail::invoke([&](sr_string_t* e) { return ::sr_cancel(db_, e); });
}

} // namespace surrealdb

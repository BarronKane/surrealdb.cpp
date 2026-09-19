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

#include <atomic>
#include <cstdio>
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
/// Statements run through `query()` are scoped together: nothing they write is
/// visible outside until `commit()`, and `cancel()` discards the lot. The
/// handle spans calls, so the caller decides what happens between statements --
/// which is the whole reason to hold one rather than send `BEGIN; ...; COMMIT;`
/// as a single query, a form that works but needs the entire transaction known
/// up front.
///
/// **This is the one place where scope really is the semantic.** An early
/// return, or any path that forgets to commit, cancels. Losing the handle
/// without doing either would leave the transaction open in the datastore
/// holding its locks until it timed out, so the destructor is not a
/// convenience.
///
/// It runs on a **forked session**, copied from the connection at `begin()`, so
/// it inherits the namespace, database and authentication in force then. A
/// later `use()` on the connection does not move a transaction already open.
///
/// Move-only, and inert once committed or cancelled -- both consume the handle
/// even when they fail, because a failed commit otherwise leaves you holding a
/// pointer that is good for nothing.
class transaction {
public:
    transaction() noexcept = default;

    transaction(transaction&& other) noexcept
        : handle_(other.handle_), alive_(std::move(other.alive_)),
          poisoned_(std::move(other.poisoned_)) {
        other.handle_ = nullptr;
    }
    /// Deleted: assigning over a live transaction would have to cancel it, and
    /// a silent rollback at an assignment is not something to make easy.
    transaction& operator=(transaction&&) = delete;
    transaction(const transaction&) = delete;
    transaction& operator=(const transaction&) = delete;

    ~transaction() {
        if (handle_ == nullptr) return;
        // The handle runs on the connection's runtime. If the connection is
        // already gone so is that runtime, and cancelling would run against a
        // dead one -- the same trade `stream` makes, for the same reason. The
        // datastore times the transaction out; a use-after-free does not
        // recover.
        if (alive_.expired()) { handle_ = nullptr; return; }
        (void)cancel();
    }

    /// True until committed or cancelled.
    [[nodiscard]] bool active() const noexcept { return handle_ != nullptr; }

    /// Run statements inside the transaction.
    ///
    /// The same `query_results` `connection::query()` returns, one entry per
    /// statement -- except that nothing written here is visible outside until
    /// `commit()`.
    ///
    /// **A failing statement does not roll the transaction back.** The error
    /// lands in that statement's slot and the choice of what to do next is
    /// yours: carry on, or `cancel()`. That choice is the reason this is a
    /// handle and not one big query string.
    ///
    /// **This is the only way statements get into a transaction**, and the
    /// asymmetry with `connection::create` and friends is deliberate on both
    /// sides of the boundary. `sr_tx_query` is the C's only transaction entry
    /// point, and its documentation says the typed variants are better answered
    /// a level up -- here. They are not offered here either, and the reason is
    /// the paragraph above rather than effort: `db.create()` returns
    /// `result<owned_object>`, folding a statement failure into the result,
    /// because on a connection there is nothing else it could mean. In a
    /// transaction a failed statement is a *decision point*, not a failed call,
    /// so a `tx.create()` of the same shape would have to throw away the thing
    /// this class exists to preserve, and one of a different shape would not be
    /// the symmetry anyone came for.
    ///
    /// That is the trade to reopen if it ever needs reopening. `single()` on
    /// the results is the fold, for a caller who wants it per statement.
    [[nodiscard]] result<query_results> query(
            const char* surql, object_arg vars = {}) noexcept {
        if (handle_ == nullptr)
            return error::local(error_code::error,
                                "query() on a transaction that was already "
                                "committed or cancelled");
        // The same guard `finish()` carries, and for the same reason: this runs
        // on the connection's runtime, which dies with the last handle onto it.
        // Omitting it here was a real hole -- the handle stays non-null when a
        // connection is destroyed, so this would have called into a freed
        // runtime while every other route reported cleanly.
        if (alive_.expired())
            return error::local(error_code::closed,
                                "the connection this transaction came from is gone");
        sr_arr_res_t* out = nullptr;
        return track(detail::invoke<query_results>(
            [&](sr_string_t* e) {
                return ::sr_tx_query(handle_, e, &out, surql,
                                     vars.raw());
            },
            [&](int n) { return query_results(owned_arr_results(out, n)); }));
    }

    // -- why there is no `raw()` here -----------------------------------------
    //
    // `connection`, `rpc`, `session_id` and `notification` all expose their C
    // pointer, and the absence of one here reads like the newest class simply
    // missing the convention. It is not.
    //
    // Those four hand out a pointer that ordinary use only ever reads --
    // `connection::raw()` is a `const sr_surreal_t*`, and no C call reachable
    // through it frees the handle. A transaction handle is *consumed*:
    // `sr_commit` and `sr_cancel` each take it and destroy it. `sr_tx_query`
    // needs it non-const, so there is no const-qualified spelling that lends
    // out the query half without lending out the destroying half, and a caller
    // who reached for `sr_commit(tx.raw(), &e)` would get a double free at the
    // next scope exit -- from a class whose whole point is that scope exit is
    // correct.
    //
    // The need an escape hatch existed for here is gone anyway: binding a
    // borrowed object as a query variable is what `object_arg` does, so
    // `query()` reaches everything `sr_tx_query` does.

    /// Commit, making everything visible together. Consumes the transaction.
    [[nodiscard]] result<void> commit() noexcept { return finish(&::sr_commit, "commit"); }

    /// Discard everything. Consumes the transaction.
    [[nodiscard]] result<void> cancel() noexcept { return finish(&::sr_cancel, "cancel"); }

private:
    friend class connection;
    transaction(sr_transaction_t* raw, std::weak_ptr<const void> alive,
                std::shared_ptr<std::atomic<bool>> poisoned) noexcept
        : handle_(raw), alive_(std::move(alive)), poisoned_(std::move(poisoned)) {}

    using finish_fn = int (*)(sr_transaction_t*, sr_string_t*);

    /// Shared by commit and cancel, which differ only in which C call runs.
    ///
    /// The handle is cleared *before* the call, because the C consumes it
    /// whether or not it succeeds -- so a caller who retries a failed commit
    /// must not reach the same pointer twice.
    result<void> finish(finish_fn fn, const char* what) noexcept {
        if (handle_ == nullptr) {
            char msg[96];
            std::snprintf(msg, sizeof(msg),
                          "%s() on a transaction that was already completed", what);
            return error::local(error_code::error, msg);
        }
        sr_transaction_t* h = handle_;
        handle_ = nullptr;

        if (alive_.expired())
            return error::local(error_code::closed,
                                "the connection this transaction came from is gone; "
                                "it was abandoned rather than run against a dead runtime");

        return track(detail::invoke([&](sr_string_t* e) { return fn(h, e); }));
    }

    /// Latches the *connection's* poison flag, which this shares -- SR_FATAL
    /// means the engine is gone, and a transaction on it is no more usable than
    /// the handle that opened it.
    template <class T>
    result<T> track(result<T> r) noexcept {
        if (!r.has_value() && r.error().is_fatal() && poisoned_)
            poisoned_->store(true, std::memory_order_release);
        return r;
    }

    sr_transaction_t* handle_{nullptr};
    std::weak_ptr<const void> alive_;
    std::shared_ptr<std::atomic<bool>> poisoned_;
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
        detail::engine_started().store(true, std::memory_order_release);
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
        detail::engine_started().store(true, std::memory_order_release);
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
    ///
    /// Shared with every handle forked from this one, and with the one it was
    /// forked from: `SR_FATAL` means the engine itself is gone, so a sibling
    /// reporting healthy would be lying.
    /// Safe on a moved-from handle, which is the reason for the branch: the
    /// flag is a shared pointer and moving hands it away, so an unguarded load
    /// would be a null dereference on a query that is otherwise harmless to
    /// ask. A handle with no flag has seen nothing.
    [[nodiscard]] bool poisoned() const noexcept {
        return poisoned_ && poisoned_->load(std::memory_order_acquire);
    }

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

    // -- sessions -----------------------------------------------------------

    /// Fork a session from this connection.
    ///
    /// The new handle talks to the same engine over the same runtime, and
    /// carries its own session state: its own `USE` namespace and database, its
    /// own `LET` variables, its own authentication. Changing any of those on
    /// one handle does not touch the other.
    ///
    /// This is how one embedded database serves several independent users --
    /// players, tenants, requests. It is **not** a second connection: no engine
    /// starts, no runtime is built, no threads are added. Needs surrealdb.c
    /// 0.3.2.
    ///
    /// The fork inherits this connection's namespace, database and
    /// authentication. `new_session()` is the same thing with the
    /// authentication cleared.
    ///
    /// Both handles are independent objects and either may be destroyed first;
    /// the engine goes away with the last one. Poisoning is shared -- see
    /// `poisoned()`.
    [[nodiscard]] result<connection> fork_session() noexcept {
        sr_surreal_t* out = nullptr;
        return track(detail::invoke<connection>(
            [&](sr_string_t* e) { return ::sr_session_fork(raw(), e, &out); },
            [&](int) { return connection(out, alive_, poisoned_); }));
    }

    /// Fork a session and clear the authentication it inherited.
    ///
    /// The namespace and database are still inherited: clearing those would
    /// hand back a handle that cannot run anything until the caller picks them
    /// again, and `use()` is right there.
    ///
    /// This is the one to reach for when the new session belongs to a different
    /// principal -- a second player, another tenant -- since inheriting a
    /// parent's credentials is exactly the leak sessions exist to prevent.
    [[nodiscard]] result<connection> new_session() noexcept {
        sr_surreal_t* out = nullptr;
        return track(detail::invoke<connection>(
            [&](sr_string_t* e) { return ::sr_session_new(raw(), e, &out); },
            [&](int) { return connection(out, alive_, poisoned_); }));
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
                                              object_arg params = {}) noexcept {
        return auth_call(&::sr_signin, level, creds, details, params);
    }

    /// Sign in at record level with the fields the SIGNIN query reads.
    ///
    /// The spelling the call usually wants: an access method plus a bag of
    /// fields, with no separate credentials.
    [[nodiscard]] result<owned_string> signin(const access& details,
                                              object_arg params) noexcept {
        return auth_call(&::sr_signin, scope::record, credentials{}, &details, params);
    }

    /// Sign up and return the token.
    ///
    /// Almost always record level, and almost always needs `params`: a SIGNUP
    /// query decides what the new record contains, so every field it reads has
    /// to arrive here.
    [[nodiscard]] result<owned_string> signup(scope level, credentials creds,
                                              const access* details = nullptr,
                                              object_arg params = {}) noexcept {
        return auth_call(&::sr_signup, level, creds, details, params);
    }

    /// Sign up at record level with the fields the SIGNUP query reads.
    [[nodiscard]] result<owned_string> signup(const access& details,
                                              object_arg params) noexcept {
        return auth_call(&::sr_signup, scope::record, credentials{}, &details, params);
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
                                              object_arg content) noexcept {
        sr_object_t made{};
        return track(detail::invoke<owned_object>(
            [&](sr_string_t* e) {
                return ::sr_create(raw(), e, &made, resource, content.raw());
            },
            [&](int) { return owned_object(made); }));
    }

    [[nodiscard]] result<void> create_discarding(const char* resource,
                                                 object_arg content) noexcept {
        return track(detail::invoke([&](sr_string_t* e) {
            return ::sr_create(raw(), e, nullptr, resource, content.raw());
        }));
    }

    [[nodiscard]] result<owned_values> update(const char* resource,
                                              object_arg content) noexcept {
        return content_call(&::sr_update, resource, content);
    }
    [[nodiscard]] result<owned_values> upsert(const char* resource,
                                              object_arg content) noexcept {
        return content_call(&::sr_upsert, resource, content);
    }
    [[nodiscard]] result<owned_values> merge(const char* resource,
                                             object_arg content) noexcept {
        return content_call(&::sr_merge, resource, content);
    }
    [[nodiscard]] result<owned_values> insert(const char* resource,
                                              object_arg content) noexcept {
        return content_call(&::sr_insert, resource, content);
    }

    [[nodiscard]] result<owned_values> relate(const char* from, const char* relation,
                                              const char* to,
                                              object_arg content) noexcept {
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
        const char* table, object_arg content) noexcept {
        return content_call(&::sr_insert_relation, table, content);
    }

    /// Call a database function -- `fn::` definitions and built-ins alike.
    ///
    /// Named `call` rather than `run` because `run()` on this class already
    /// executes a built query. A scalar result comes back as a one-element
    /// array.
    [[nodiscard]] result<owned_values> call(const char* function_name,
                                            array_arg args) noexcept {
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
        const char* surql, object_arg vars = {}) noexcept {
        sr_arr_res_t* out = nullptr;
        return track(detail::invoke<query_results>(
            [&](sr_string_t* e) {
                return ::sr_query(raw(), e, &out, surql, vars.raw());
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
            [&](int) {
                return stream(out, std::weak_ptr<const void>(alive_), poisoned_);
            }));
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
    /// Both routes retire the subscription, so pick by what you are holding:
    ///
    /// - a `stream` from `live()` -- call `stream::close()`, which frees the
    ///   reader and retires the query in one step, and needs no id;
    /// - an id and no stream, such as a bare `LIVE SELECT` run through
    ///   `query()` -- call this.
    ///
    /// Calling both is harmless, just unnecessary. Any stream reading the
    /// killed subscription is told: it reports its end rather than going quiet.
    ///
    /// None of that used to be true. Freeing a reader left the subscription
    /// registered, and a killed stream stayed open and silent forever, so
    /// teardown needed both calls and a blocked reader could not be released at
    /// all. The anchored surrealdb.c carries the fixes; `INFO FOR TABLE`'s
    /// `lives` is where the datastore half is visible if you want to watch it.
    ///
    /// On an `rpc` context, prefer `rpc::kill_on()` over writing `KILL` into
    /// query text -- see its note.
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

    /// Begin a transaction.
    ///
    /// Statements run on the returned handle are scoped together and stay
    /// invisible outside it until committed. It cancels on scope exit unless
    /// you commit, and losing it without doing either would leave the
    /// transaction open in the datastore holding its locks -- so let it own its
    /// scope.
    ///
    ///     auto tx = std::move(db.begin()).value();
    ///     if (!tx.query("UPDATE account:a SET bal -= 10")) return;   // cancels
    ///     if (!tx.query("UPDATE account:b SET bal += 10")) return;   // cancels
    ///     auto done = tx.commit();
    ///
    /// This was `= delete`d for a while, and the reason is worth keeping: the C
    /// used to send `BEGIN`, `COMMIT` and `CANCEL` each as its own
    /// single-statement query, and a bare `BEGIN` is a complete query -- so the
    /// transaction opened and closed inside that one call and nothing after it
    /// was scoped, with every call still reporting success. It now carries a
    /// handle that threads a transaction id through each statement, which is
    /// what makes any of this real.
    ///
    /// The transaction runs on a forked session and inherits this connection's
    /// namespace, database and authentication as they are *now*; a later
    /// `use()` here does not move it.
    [[nodiscard]] result<transaction> begin() noexcept {
        sr_transaction_t* out = nullptr;
        return track(detail::invoke<transaction>(
            [&](sr_string_t* e) { return ::sr_begin(raw(), e, &out); },
            [&](int) {
                return transaction(out, std::weak_ptr<const void>(alive_), poisoned_);
            }));
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

    /// A fork: a new handle onto an engine this one already owns.
    ///
    /// It shares both pieces of shared state deliberately, because the C does:
    ///
    /// - the **liveness token**, so a stream opened on any handle stays
    ///   killable while any handle survives. The runtime is reference-counted
    ///   on the C side and dies with the last handle, so a per-handle token
    ///   would expire early and make a perfectly valid stream leak.
    /// - the **poison flag**, because `SR_FATAL` means the engine is gone. A
    ///   fork that reported healthy while talking to a dead engine would be
    ///   worse than useless.
    connection(sr_surreal_t* raw, std::shared_ptr<const void> alive,
               std::shared_ptr<std::atomic<bool>> poisoned) noexcept
        : db_(raw), alive_(std::move(alive)), poisoned_(std::move(poisoned)) {}

    /// Latch the poison flag from any result that reports SR_FATAL.
    template <class T>
    result<T> track(result<T> r) noexcept {
        if (!r.has_value() && r.error().is_fatal() && poisoned_)
            poisoned_->store(true, std::memory_order_release);
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
                                      object_arg content) noexcept {
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
                                   object_arg params) noexcept {
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
                          params.raw());
            },
            [&](int) { return owned_string(token); }));
    }

    owned_connection db_;

    /// Liveness token handed to streams as a weak_ptr. It dies with this
    /// object, which is how a stream can tell that its connection -- and the
    /// runtime it borrows -- is gone.
    std::shared_ptr<const void> alive_{std::make_shared<const char>('\0')};

    /// Shared with forks; see `poisoned()`.
    std::shared_ptr<std::atomic<bool>> poisoned_{
        std::make_shared<std::atomic<bool>>(false)};
};

// ---------------------------------------------------------------------------

} // namespace surrealdb

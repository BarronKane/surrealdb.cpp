#pragma once

/// @file options.hpp
/// Connection options and the capability sandbox.
///
/// A thin, safe face over `sr_option_t`. The C struct is a plain aggregate
/// whose zero value means "SurrealDB's default" for every field; this keeps
/// that property and adds two things C cannot: the string arrays backing a
/// target set are owned, so they cannot dangle, and the whole thing is
/// move-only so a copy cannot silently share them.

#include "config.hpp"
#include "detail/c_api.hpp"
#include "error.hpp"
#include "poll.hpp"

#include <cstdint>
#include <initializer_list>
#include <atomic>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace surrealdb {

/// Which members of a capability set are allowed or denied.
enum class target_mode : int {
    inherit = SR_TARGET_DEFAULT,  ///< leave SurrealDB's default in place
    none    = SR_TARGET_NONE,
    some    = SR_TARGET_SOME,
    all     = SR_TARGET_ALL,
};

enum class toggle : int {
    inherit = SR_TOGGLE_DEFAULT,
    on      = SR_TOGGLE_ON,
    off     = SR_TOGGLE_OFF,
};

/// One capability target set, owning its names.
///
/// In C the caller must keep the `const char*` array alive until connect; here
/// the strings are owned and the `const char*` view is rebuilt on demand, so
/// there is no lifetime rule to get wrong.
class target_set {
public:
    target_set() = default;

    static target_set inherit() { return target_set(target_mode::inherit); }
    static target_set none() { return target_set(target_mode::none); }
    static target_set all() { return target_set(target_mode::all); }

    /// An explicit list. An empty list is equivalent to `none()`.
    static target_set only(std::initializer_list<std::string> names) {
        target_set t(target_mode::some);
        t.names_.assign(names.begin(), names.end());
        return t;
    }

    target_set& add(std::string name) {
        mode_ = target_mode::some;
        names_.push_back(std::move(name));
        return *this;
    }

    [[nodiscard]] target_mode mode() const noexcept { return mode_; }
    [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }

private:
    explicit target_set(target_mode m) : mode_(m) {}

    target_mode mode_{target_mode::inherit};
    std::vector<std::string> names_;
};

/// The capability sandbox.
///
/// Everything defaults to `inherit`, so a default-constructed `capabilities`
/// changes nothing. Where an allow and a deny set name the same thing, deny
/// wins -- SurrealDB's rule, not this library's.
struct capabilities {
    toggle scripting{toggle::inherit};
    toggle guest_access{toggle::inherit};
    toggle live_query_notifications{toggle::inherit};

    target_set allow_functions, deny_functions;
    /// Outbound network access is **denied** by SurrealDB's defaults.
    target_set allow_network, deny_network;
    target_set allow_rpc_methods, deny_rpc_methods;
    target_set allow_http_routes, deny_http_routes;
    /// `"gql"` is required by the gql/graphql RPC methods, `"files"` by
    /// `file://` values.
    target_set allow_experimental, deny_experimental;
    target_set allow_arbitrary_query, deny_arbitrary_query;
    target_set allow_eval_query, deny_eval_query;
};

/// Connection options.
class options {
public:
    options() = default;

    options& query_timeout(std::uint8_t seconds) noexcept {
        query_timeout_ = seconds;
        return *this;
    }
    options& transaction_timeout(std::uint8_t seconds) noexcept {
        transaction_timeout_ = seconds;
        return *this;
    }
    /// Directory in which RPC sessions are persisted. Empty disables it.
    ///
    /// **These files hold credentials.** A session is serialised whole, and a
    /// session carries its authentication token, its record-authentication data
    /// and its variables -- written as plain JSON, neither encrypted nor
    /// obfuscated. Anything that can read the file can replay the session.
    ///
    /// surrealdb.c restricts the directory and its files to the current user
    /// (0700/0600 on unix; elsewhere the inherited ACL is all there is), which
    /// is sufficient on a server -- the case upstream built this for. It is not
    /// disk encryption. Keep the directory off shared or synced volumes, and
    /// think hard before enabling it at all on hardware the end user controls,
    /// where "the current user" and "the attacker" are the same account.
    ///
    /// Left empty, nothing is written and sessions live only as long as the
    /// context. That is the right default for a client.
    options& session_dir(std::string dir) {
        session_dir_ = std::move(dir);
        return *this;
    }

    // -- runtime shape ------------------------------------------------------
    //
    // These size the tokio runtime a context builds for itself. Every one
    // defaults to zero, which means "surrealdb.c's default" rather than
    // "nothing" -- a default-constructed `options` still changes nothing.

    /// Worker threads for this context. Zero takes the library default
    /// (`SR_DEFAULT_WORKER_THREADS`), which is deliberately *not* the core
    /// count: an embedded database inside a host application has no business
    /// claiming every core before a query runs.
    options& worker_threads(int n) noexcept { worker_threads_ = n; return *this; }

    /// Run the whole runtime on one thread. Not a count but a mode -- the
    /// scheduler itself becomes single-threaded, which is the genuinely slim
    /// option for intermittent work. Overrides `worker_threads`.
    options& current_thread(bool on) noexcept { current_thread_ = on; return *this; }

    /// Ceiling on lazily-spawned blocking threads. Zero means tokio's 512.
    /// A cap, not residency: they spawn on demand and retire after
    /// `thread_keep_alive`.
    options& max_blocking_threads(int n) noexcept {
        max_blocking_threads_ = n; return *this;
    }

    /// How long an idle blocking thread lingers. Zero means tokio's 10s.
    /// Lower means less sawtooth after a burst.
    template <class Rep, class Period>
    options& thread_keep_alive(std::chrono::duration<Rep, Period> d) noexcept {
        thread_keep_alive_ms_ = detail::to_timeout_ms(d);
        return *this;
    }

    /// Stack size per worker thread, in bytes. Zero means the platform
    /// default, which on Linux reserves 8 MiB of address space per thread.
    options& thread_stack_size(int bytes) noexcept {
        thread_stack_size_ = bytes; return *this;
    }

    /// Skip the IO driver. Free for an embedded-only context, but it is what
    /// `http://` and `ws://` endpoints -- and SurrealQL's `http::*` functions
    /// -- are built on, so a context that reaches the network must leave this
    /// alone. Off by default for that reason.
    options& disable_io(bool on) noexcept { disable_io_ = on; return *this; }

    /// Directory for temporary files. Empty means the platform default, which
    /// is not always right, or writable at all, on console and mobile targets.
    options& temporary_directory(std::string dir) {
        temporary_directory_ = std::move(dir); return *this;
    }

    /// Log queries slower than this. Zero disables it. Ignored by `rpc`, which
    /// does not build the datastore directly.
    ///
    /// **Slow-query logs include bound parameters**, and parameters are how
    /// credentials travel -- a `signin` carries its password as one. The log
    /// goes wherever the host's `tracing` subscriber points, which on a client
    /// machine may be a file that outlives the process. Enabling this is a
    /// decision about credential handling, not about verbosity.
    template <class Rep, class Period>
    options& slow_log(std::chrono::duration<Rep, Period> d) noexcept {
        slow_log_ms_ = detail::to_timeout_ms(d);
        return *this;
    }

    [[nodiscard]] surrealdb::capabilities& capabilities() noexcept { return caps_; }
    [[nodiscard]] const surrealdb::capabilities& capabilities() const noexcept { return caps_; }

    /// Materialise the C view.
    ///
    /// The returned `sr_option_t` points into `storage`, which must outlive the
    /// call that consumes it. Keeping the pointers in a separate object rather
    /// than inside `options` is what makes the lifetime visible at the call
    /// site instead of implicit.
    struct c_view {
        sr_option_t opts{};
        // Stable backing for every `const char*` handed to C.
        std::vector<std::vector<const char*>> ptr_lists;
        std::string session_dir;
        std::string temporary_directory;
    };

    [[nodiscard]] c_view to_c() const {
        c_view v;
        v.opts.query_timeout = query_timeout_;
        v.opts.transaction_timeout = transaction_timeout_;

        v.session_dir = session_dir_;
        v.opts.session_dir = v.session_dir.empty() ? nullptr : v.session_dir.c_str();

        v.temporary_directory = temporary_directory_;
        v.opts.temporary_directory =
            v.temporary_directory.empty() ? nullptr : v.temporary_directory.c_str();

        v.opts.worker_threads = worker_threads_;
        v.opts.current_thread = current_thread_;
        v.opts.max_blocking_threads = max_blocking_threads_;
        v.opts.thread_keep_alive_ms = thread_keep_alive_ms_;
        v.opts.thread_stack_size = thread_stack_size_;
        v.opts.disable_io = disable_io_;
        v.opts.slow_log_ms = slow_log_ms_;

        v.opts.capabilities.scripting = static_cast<sr_toggle_t>(caps_.scripting);
        v.opts.capabilities.guest_access = static_cast<sr_toggle_t>(caps_.guest_access);
        v.opts.capabilities.live_query_notifications =
            static_cast<sr_toggle_t>(caps_.live_query_notifications);

        // Reserved up front: a reallocation would invalidate pointers already
        // written into the option struct.
        v.ptr_lists.reserve(14);

        auto fill = [&v](sr_targets_t& out, const target_set& in) {
            out.mode = static_cast<sr_target_mode_t>(in.mode());
            out.items = nullptr;
            out.len = 0;
            if (in.mode() != target_mode::some || in.names().empty()) return;
            v.ptr_lists.emplace_back();
            auto& list = v.ptr_lists.back();
            list.reserve(in.names().size());
            for (const std::string& n : in.names()) list.push_back(n.c_str());
            out.items = list.data();
            out.len = static_cast<int>(list.size());
        };

        auto& c = v.opts.capabilities;
        fill(c.allow_functions, caps_.allow_functions);
        fill(c.deny_functions, caps_.deny_functions);
        fill(c.allow_network, caps_.allow_network);
        fill(c.deny_network, caps_.deny_network);
        fill(c.allow_rpc_methods, caps_.allow_rpc_methods);
        fill(c.deny_rpc_methods, caps_.deny_rpc_methods);
        fill(c.allow_http_routes, caps_.allow_http_routes);
        fill(c.deny_http_routes, caps_.deny_http_routes);
        fill(c.allow_experimental, caps_.allow_experimental);
        fill(c.deny_experimental, caps_.deny_experimental);
        fill(c.allow_arbitrary_query, caps_.allow_arbitrary_query);
        fill(c.deny_arbitrary_query, caps_.deny_arbitrary_query);
        fill(c.allow_eval_query, caps_.allow_eval_query);
        fill(c.deny_eval_query, caps_.deny_eval_query);
        return v;
    }

private:
    std::uint8_t query_timeout_{0};
    std::uint8_t transaction_timeout_{0};
    std::string session_dir_;
    std::string temporary_directory_;
    int worker_threads_{0};
    int max_blocking_threads_{0};
    int thread_keep_alive_ms_{0};
    int thread_stack_size_{0};
    int slow_log_ms_{0};
    bool current_thread_{false};
    bool disable_io_{false};
    surrealdb::capabilities caps_;
};

// ---------------------------------------------------------------------------
// Process-wide settings
// ---------------------------------------------------------------------------

namespace detail {

/// Set the first time any context is opened.
///
/// A function-local static so this stays header-only and single-instance. It
/// exists only so `runtime_init` can report the one mistake the C cannot.
inline std::atomic<bool>& engine_started() noexcept {
    static std::atomic<bool> started{false};
    return started;
}

} // namespace detail

/// Settings that belong to the process, not to a context.
///
/// Separate from `options` because the lifetime is different, and the C draws
/// the same line for the same reason: a per-context field carrying a
/// process-global setting is a trap, because the second context's value is
/// silently ignored and nothing at the call site says so.
class runtime_options {
public:
    runtime_options() = default;

    /// Size of SurrealDB's shared blocking pool. Zero leaves it alone.
    ///
    /// Worth setting on a host application even at the default value's own
    /// size. Upstream spends one worker per core on machines with 16 or more
    /// cores and 16 below that -- so a 32-core machine spends 32 threads here
    /// before a single query runs -- and, worse, it *pins* one worker per core
    /// when the size equals the core count and that count is at least 16, which
    /// fights any engine managing its own affinity. Any value that differs from
    /// the core count drops the pinning.
    ///
    /// SurrealDB clamps it to a minimum of 4.
    runtime_options& kvs_threadpool_size(int n) noexcept {
        kvs_threadpool_size_ = n;
        return *this;
    }

    [[nodiscard]] sr_runtime_options_t to_c() const noexcept {
        sr_runtime_options_t o{};
        o.kvs_threadpool_size = kvs_threadpool_size_;
        return o;
    }

private:
    int kvs_threadpool_size_{0};
};

/// Apply process-wide settings. Call once, before opening anything.
///
/// `true` means the settings were applied; `false` means there was nothing to
/// do, which is what a default-constructed `runtime_options` produces.
///
/// **This is not idempotent and cannot be.** SurrealDB builds its blocking pool
/// once per process, on first use, from an environment variable read behind a
/// `LazyLock`. So this has to run before the first `connection::connect` or
/// `rpc::connect`. Afterwards it does nothing -- and the C, having no way to
/// know the pool was already built, cannot tell you that it did nothing.
///
/// This wrapper can, for the in-process case: it remembers whether a context
/// has been opened and fails rather than succeeding silently. That does not
/// cover a pool built by something else in the same process, so a `true` here
/// is still "the variable was set", not "the pool is now this size". Call it
/// first and the distinction never comes up.
[[nodiscard]] inline result<bool> runtime_init(const runtime_options& opts) noexcept {
    if (detail::engine_started().load(std::memory_order_acquire)) {
        return error::local(error_code::error,
                            "runtime_init() after a context was already opened: "
                            "SurrealDB builds its blocking pool once, on first "
                            "use, so this would have been silently ignored. Call "
                            "it before the first connect().");
    }

    const sr_runtime_options_t c = opts.to_c();
    sr_string_t err = nullptr;
    const int rc = ::sr_runtime_init(&err, &c);
    if (rc < 0) return error::adopt(rc, err);
    if (err != nullptr) ::sr_string_free(err);
    return rc > 0;
}

/// Apply nothing, and only record that the process is past the point where
/// applying would work. Rarely wanted; present so the no-op case is spellable.
[[nodiscard]] inline result<bool> runtime_init() noexcept {
    return runtime_init(runtime_options{});
}

} // namespace surrealdb

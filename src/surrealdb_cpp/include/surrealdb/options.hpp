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

#include <cstdint>
#include <initializer_list>
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
    options& session_dir(std::string dir) {
        session_dir_ = std::move(dir);
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
    };

    [[nodiscard]] c_view to_c() const {
        c_view v;
        v.opts.query_timeout = query_timeout_;
        v.opts.transaction_timeout = transaction_timeout_;

        v.session_dir = session_dir_;
        v.opts.session_dir = v.session_dir.empty() ? nullptr : v.session_dir.c_str();

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
    surrealdb::capabilities caps_;
};

} // namespace surrealdb

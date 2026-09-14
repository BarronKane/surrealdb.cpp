#pragma once

/// @file dsl/arena.hpp
/// The buffer every query is assembled into.
///
/// One `std::string` reserved up front plus one object of bind variables. No
/// clause allocates; a guard is a recorded offset, not an object with storage.
/// Both the scope-guard surface (B) and the fluent surface (A) write here --
/// A has no assembly of its own, which is what keeps the two from drifting.
///
/// **Values are bound, never interpolated.** A literal is written as `$v0` and
/// the value goes into the variables object, so a string containing a quote or
/// a semicolon cannot change the shape of the statement. That is the reason
/// this exists rather than `std::string` concatenation at the call site.

#include "../config.hpp"
#include "../detail/c_api.hpp"
#include "../error.hpp"
#include "../object.hpp"
#include "../value.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace surrealdb {
namespace dsl {

/// A finished query: the statement text and the variables it refers to.
class built_query {
public:
    built_query() = default;
    built_query(std::string text, object_builder vars) noexcept
        : text_(std::move(text)), vars_(std::move(vars)) {}

    [[nodiscard]] const std::string& text() const noexcept { return text_; }
    [[nodiscard]] const object_builder& vars() const noexcept { return vars_; }
    [[nodiscard]] const char* c_str() const noexcept { return text_.c_str(); }

private:
    std::string text_;
    object_builder vars_;
};

/// Assembly buffer plus bind variables.
class arena {
public:
    arena() { buf_.reserve(256); }

    arena(arena&&) noexcept = default;
    arena& operator=(arena&&) noexcept = default;
    arena(const arena&) = delete;
    arena& operator=(const arena&) = delete;

    // -- raw writing --------------------------------------------------------

    void put(const char* s) { buf_ += s; }
    void put(std::string_view s) { buf_.append(s.data(), s.size()); }
    void put(char c) { buf_ += c; }

    /// A space, unless the buffer is empty or already ends in one.
    void space() {
        if (!buf_.empty() && buf_.back() != ' ' && buf_.back() != '(') buf_ += ' ';
    }

    /// A keyword, spaced on both sides. Both matter: without the trailing
    /// space `WHERE` and the field name run together into `WHEREcity`.
    void keyword(const char* kw) { space(); put(kw); put(' '); }

    /// Where the next write will land. A guard records this so it can tell
    /// whether its clause ended up empty.
    [[nodiscard]] std::size_t mark() const noexcept { return buf_.size(); }
    [[nodiscard]] bool grew_since(std::size_t m) const noexcept { return buf_.size() > m; }

    /// Discard everything written since `m`. Used to drop a clause that turned
    /// out to have no content -- an empty `WHERE` is a syntax error, and
    /// writing one and then removing it is cheaper than looking ahead.
    void rewind(std::size_t m) {
        if (m <= buf_.size()) buf_.resize(m);
    }

    // -- binding ------------------------------------------------------------

    /// Bind a value and write its placeholder.
    ///
    /// The name is generated, so two calls never collide and a caller cannot
    /// accidentally shadow a variable the query already uses.
    void bind(value v) {
        const std::string name = next_name();
        vars_.set(name.c_str(), v);
        put('$');
        put(name);
    }

    /// Any integer, promoted.
    ///
    /// Templated rather than a fixed `std::int64_t` overload: `f("age") > 30`
    /// hands over an `int`, and requiring a cast at every literal is the kind
    /// of papercut that makes a DSL not worth using. `bool` is excluded so it
    /// keeps its own overload rather than promoting to 0/1.
    template <class T,
              std::enable_if_t<std::is_integral<T>::value &&
                                   !std::is_same<T, bool>::value,
                               int> = 0>
    void bind(T v) {
        const std::string name = next_name();
        vars_.set(name.c_str(), static_cast<std::int64_t>(v));
        put('$');
        put(name);
    }

    /// Any floating-point value, widened.
    template <class T,
              std::enable_if_t<std::is_floating_point<T>::value, int> = 0>
    void bind(T v) {
        const std::string name = next_name();
        vars_.set(name.c_str(), static_cast<double>(v));
        put('$');
        put(name);
    }

    void bind(bool v) {
        const std::string name = next_name();
        vars_.set(name.c_str(), v);
        put('$');
        put(name);
    }

    void bind(const char* v) {
        const std::string name = next_name();
        vars_.set(name.c_str(), v);
        put('$');
        put(name);
    }

    void bind(const std::string& v) { bind(v.c_str()); }

    // -- identifiers --------------------------------------------------------

    /// Write a table, field or record id.
    ///
    /// Identifiers cannot be bound -- SurrealQL takes them as syntax, not as
    /// values -- so they are validated instead. Anything outside the character
    /// set SurrealDB accepts unquoted is rejected, which closes the injection
    /// route that binding closes for values.
    void identifier(std::string_view id) {
        if (!is_plain_identifier(id)) {
            fail("not a valid identifier: ");
            return;
        }
        put(id);
    }

    [[nodiscard]] static bool is_plain_identifier(std::string_view id) noexcept {
        if (id.empty()) return false;
        for (char c : id) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == ':' ||
                            c == '.' || c == '*' || c == '-' || c == '>' || c == '<';
            if (!ok) return false;
        }
        return true;
    }

    // -- error state --------------------------------------------------------

    /// Record a construction error.
    ///
    /// Building cannot throw and a destructor cannot report, so a misuse is
    /// latched here and surfaces when the query is built. The alternative --
    /// asserting -- would turn a recoverable mistake into a crash in release.
    void fail(std::string_view why) {
        if (failed_) return;
        failed_ = true;
        reason_ = std::string(why);
    }

    void fail(std::string_view why, std::string_view detail) {
        if (failed_) return;
        failed_ = true;
        reason_ = std::string(why);
        reason_.append(detail.data(), detail.size());
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const std::string& reason() const noexcept { return reason_; }

    // -- child tracking -----------------------------------------------------
    //
    // A guard registers itself while it is alive so its parent can refuse to
    // write. Balance falls out of the destructors; *ordering* does not, and
    // this is what catches it.

    void push_child() noexcept { ++depth_; }
    void pop_child() noexcept { if (depth_) --depth_; }

    /// How deeply nested the innermost live guard is.
    ///
    /// A guard compares this against the depth it recorded when it was
    /// constructed. Equal means it is the innermost one and may write; greater
    /// means something opened inside it and has not closed, so writing here
    /// would put the text in the wrong clause. Counting alone is not enough --
    /// a guard must not be refused for its own presence.
    [[nodiscard]] int depth() const noexcept { return depth_; }

    void fail_ordering(const char* what) {
        fail("wrote to an outer clause while an inner one was still open: ", what);
    }

    // -- result -------------------------------------------------------------

    [[nodiscard]] std::string& buffer() noexcept { return buf_; }
    [[nodiscard]] object_builder& variables() noexcept { return vars_; }

private:
    std::string next_name() {
        std::string n = "v";
        n += std::to_string(next_var_++);
        return n;
    }

    std::string buf_;
    object_builder vars_;
    std::string reason_;
    int next_var_{0};
    int depth_{0};
    bool failed_{false};
};

} // namespace dsl
} // namespace surrealdb

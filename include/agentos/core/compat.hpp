#pragma once
// ============================================================
// AgentOS :: Compatibility Layer
// 用项目自有命名空间实现 C++23 缺失特性的 polyfill
// （GCC 11 / C++20 兼容，不污染 std 命名空间）
// ============================================================
#include <cstdarg>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace agentos {

// ─────────────────────────────────────────────────────────────
// Expected<T, E>  —  类似 std::expected（C++23）
// ─────────────────────────────────────────────────────────────

template<typename E>
struct Unexpected {
    E err;
    explicit Unexpected(E e) : err(std::move(e)) {}
};

template<typename E>
Unexpected<std::decay_t<E>> make_unexpected(E&& e) {
    return Unexpected<std::decay_t<E>>{std::forward<E>(e)};
}

template<typename T, typename E>
class [[nodiscard]] Expected {
public:
    using value_type = T;
    using error_type = E;

    // 值构造
    Expected(T val) : data_(std::in_place_index<0>, std::move(val)) {}

    ~Expected() noexcept = default;

    // 错误构造
    Expected(Unexpected<E> u)
        : data_(std::in_place_index<1>, std::move(u.err)) {}

    bool has_value() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    T& value() & {
        if (!has_value()) throw std::runtime_error("Expected has no value");
        return std::get<0>(data_);
    }
    const T& value() const & {
        if (!has_value()) throw std::runtime_error("Expected has no value");
        return std::get<0>(data_);
    }
    T value() && {
        if (!has_value()) throw std::runtime_error("Expected has no value");
        return std::move(std::get<0>(data_));
    }

    T* operator->()             { return &std::get<0>(data_); }
    const T* operator->() const { return &std::get<0>(data_); }
    T& operator*()              { return std::get<0>(data_); }
    const T& operator*() const  { return std::get<0>(data_); }

    E& error()             { return std::get<1>(data_); }
    const E& error() const { return std::get<1>(data_); }

    template<typename U>
    T value_or(U&& def) const & {
        return has_value() ? std::get<0>(data_) : static_cast<T>(std::forward<U>(def));
    }

    // Monadic operations (C++23)
    template<typename F>
    auto and_then(F&& f) & {
        using Ret = std::invoke_result_t<F, T&>;
        if (has_value()) return std::invoke(std::forward<F>(f), value());
        return Ret(make_unexpected(error()));
    }

    template<typename F>
    auto and_then(F&& f) const & {
        using Ret = std::invoke_result_t<F, const T&>;
        if (has_value()) return std::invoke(std::forward<F>(f), value());
        return Ret(make_unexpected(error()));
    }

    template<typename F>
    auto transform(F&& f) & {
        using U = std::invoke_result_t<F, T&>;
        if constexpr (std::is_void_v<U>) {
            if (has_value()) {
                std::invoke(std::forward<F>(f), value());
                return Expected<void, E>();
            }
            return Expected<void, E>(make_unexpected(error()));
        } else {
            if (has_value()) return Expected<U, E>(std::invoke(std::forward<F>(f), value()));
            return Expected<U, E>(make_unexpected(error()));
        }
    }

    template<typename F>
    auto transform(F&& f) const & {
        using U = std::invoke_result_t<F, const T&>;
        if constexpr (std::is_void_v<U>) {
            if (has_value()) {
                std::invoke(std::forward<F>(f), value());
                return Expected<void, E>();
            }
            return Expected<void, E>(make_unexpected(error()));
        } else {
            if (has_value()) return Expected<U, E>(std::invoke(std::forward<F>(f), value()));
            return Expected<U, E>(make_unexpected(error()));
        }
    }

    template<typename F>
    auto or_else(F&& f) & {
        if (has_value()) return *this;
        return std::invoke(std::forward<F>(f), error());
    }

    template<typename F>
    auto or_else(F&& f) const & {
        if (has_value()) return *this;
        return std::invoke(std::forward<F>(f), error());
    }

    template<typename F>
    auto or_else(F&& f) && {
        if (has_value()) return std::move(*this);
        return std::invoke(std::forward<F>(f), std::move(error()));
    }

private:
    std::variant<T, E> data_;
};

// void 特化
template<typename E>
class [[nodiscard]] Expected<void, E> {
public:
    Expected() noexcept : has_value_(true) {}
    Expected(Unexpected<E> u) : has_value_(false), err_(std::move(u.err)) {}

    ~Expected() noexcept = default;

    bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    void value() const {
        if (!has_value_)
            throw std::runtime_error("Expected<void> has error");
    }
    E& error()             { return err_; }
    const E& error() const { return err_; }

    // Monadic operations for void (C++23)
    template<typename F>
    auto and_then(F&& f) const & {
        using Ret = std::invoke_result_t<F>;
        if (has_value_) return std::invoke(std::forward<F>(f));
        return Ret(make_unexpected(err_));
    }

    template<typename F>
    auto transform(F&& f) const & {
        using U = std::invoke_result_t<F>;
        using Ret = Expected<U, E>;
        if (has_value_) return Ret(std::invoke(std::forward<F>(f)));
        return Ret(make_unexpected(err_));
    }

    template<typename F>
    auto or_else(F&& f) const & {
        if (has_value_) return *this;
        return std::invoke(std::forward<F>(f), err_);
    }

private:
    bool has_value_;
    E err_{};
};

// ─────────────────────────────────────────────────────────────
// fmt::format  —  简单 {} 风格格式化
// ─────────────────────────────────────────────────────────────

namespace fmt {

namespace detail {

template<typename T>
std::string to_str(const T& v) {
    std::ostringstream ss; ss << v; return ss.str();
}
template<>
inline std::string to_str<std::string>(const std::string& v) { return v; }
template<>
inline std::string to_str<std::string_view>(const std::string_view& v) {
    return std::string(v);
}
template<>
inline std::string to_str<bool>(const bool& v) { return v ? "true" : "false"; }

inline std::string format_impl(std::string_view fmt_str) {
    return std::string(fmt_str);
}

template<typename Arg, typename... Rest>
std::string format_impl(std::string_view fmt_str, Arg&& arg, Rest&&... rest) {
    auto pos = fmt_str.find('{');
    if (pos == std::string_view::npos)
        return std::string(fmt_str);
    // 找到结束 }
    auto end = fmt_str.find('}', pos);
    if (end == std::string_view::npos)
        return std::string(fmt_str);

    std::string result;
    result += fmt_str.substr(0, pos);
    result += to_str(std::forward<Arg>(arg));
    result += format_impl(fmt_str.substr(end + 1),
                          std::forward<Rest>(rest)...);
    return result;
}

} // namespace detail

template<typename... Args>
std::string format(std::string_view fmt_str, Args&&... args) {
    return detail::format_impl(fmt_str, std::forward<Args>(args)...);
}

} // namespace fmt

// ─────────────────────────────────────────────────────────────
// source_location  —  简化版（GCC 11 已内置，这里是备份）
// ─────────────────────────────────────────────────────────────

struct SourceLocation {
    const char* file;
    int         line;

    static SourceLocation current(
        const char* f = __builtin_FILE(),
        int l         = __builtin_LINE()) noexcept {
        return {f, l};
    }
    const char* file_name() const noexcept { return file; }
    int line_number()       const noexcept { return line; }
};

} // namespace agentos

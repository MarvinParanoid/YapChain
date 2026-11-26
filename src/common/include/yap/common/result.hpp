// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace yap {

/// Error wrapper used to construct a failed Result. std::expected stays unused: Clang 17/18 with libstdc++
/// (the supported Clang toolchains) do not provide it (ADR-0030).
template <class E>
struct Unexpected {
    E error;
};

template <class E>
[[nodiscard]] constexpr Unexpected<std::decay_t<E>> unexpected(E&& e) {
    return Unexpected<std::decay_t<E>>{std::forward<E>(e)};
}

/// Value type of a Result that carries no value (a status).
struct Ok {
    friend constexpr bool operator==(Ok, Ok) noexcept { return true; }
};

/// Thrown only on programmer error: reading the wrong alternative of a Result.
class BadResultAccess : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

template <class T, class E>
class [[nodiscard]] Result {
public:
    Result(const T& value) : storage_(std::in_place_index<0>, value) {}             // NOLINT
    Result(T&& value) : storage_(std::in_place_index<0>, std::move(value)) {}        // NOLINT
    Result(Unexpected<E> e) : storage_(std::in_place_index<1>, std::move(e.error)) {}  // NOLINT

    [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & {
        check_value();
        return std::get<0>(storage_);
    }
    [[nodiscard]] const T& value() const& {
        check_value();
        return std::get<0>(storage_);
    }
    [[nodiscard]] T&& value() && {
        check_value();
        return std::get<0>(std::move(storage_));
    }
    [[nodiscard]] const E& error() const& {
        if (has_value()) {
            throw BadResultAccess("Result::error() called on a value");
        }
        return std::get<1>(storage_);
    }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

private:
    void check_value() const {
        if (!has_value()) {
            throw BadResultAccess("Result::value() called on an error");
        }
    }

    std::variant<T, E> storage_;
};

template <class E>
using Status = Result<Ok, E>;

}  // namespace yap

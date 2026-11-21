#pragma once

#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace veloplay {

enum class ErrorCode {
    InvalidArgument,
    InvalidData,
    Network,
    Timeout,
    Canceled,
    Rejected, // the console answered, but said no
    Crypto,
    Io,
};

constexpr const char* toString(ErrorCode code) {
    switch (code) {
        case ErrorCode::InvalidArgument: return "invalid_argument";
        case ErrorCode::InvalidData: return "invalid_data";
        case ErrorCode::Network: return "network";
        case ErrorCode::Timeout: return "timeout";
        case ErrorCode::Canceled: return "canceled";
        case ErrorCode::Rejected: return "rejected";
        case ErrorCode::Crypto: return "crypto";
        case ErrorCode::Io: return "io";
    }
    return "unknown";
}

struct Error {
    ErrorCode code;
    std::string message;
};

/// Either a value or an Error. Deliberately small; std::expected needs C++23.
template <class T>
class [[nodiscard]] Result {
public:
    // Two overloads instead of one by-value parameter: `return localObject;`
    // then moves on every compiler, which move-only types such as sockets need.
    Result(const T& value) : data_(std::in_place_index<0>, value) {}
    Result(T&& value) : data_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : data_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const { return data_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() & { assert(ok()); return std::get<0>(data_); }
    const T& value() const& { assert(ok()); return std::get<0>(data_); }
    T&& value() && { assert(ok()); return std::get<0>(std::move(data_)); }

    const Error& error() const { assert(!ok()); return std::get<1>(data_); }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

private:
    std::variant<T, Error> data_;
};

template <>
class [[nodiscard]] Result<void> {
public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool ok() const { return !error_.has_value(); }
    explicit operator bool() const { return ok(); }
    const Error& error() const { assert(!ok()); return *error_; }

private:
    std::optional<Error> error_;
};

} // namespace veloplay

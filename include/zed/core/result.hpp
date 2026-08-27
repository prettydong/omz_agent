#pragma once

#include <string>
#include <utility>
#include <variant>

namespace zed::core {

enum class ErrorCode {
    invalid_argument,
    cancelled,
    timeout,
    not_found,
    conflict,
    model_error,
    tool_error,
    session_error,
    context_error,
    internal,
};

struct Error {
    ErrorCode code{ErrorCode::internal};
    std::string message;
};

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const { return std::holds_alternative<T>(value_); }
    [[nodiscard]] explicit operator bool() const { return has_value(); }

    [[nodiscard]] const T& value() const { return std::get<T>(value_); }
    [[nodiscard]] T& value() { return std::get<T>(value_); }
    [[nodiscard]] const Error& error() const { return std::get<Error>(value_); }
    [[nodiscard]] Error& error() { return std::get<Error>(value_); }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Error error) : value_(std::move(error)) {}

    std::variant<T, Error> value_;
};

template <>
class Result<void> {
public:
    static Result success() { return Result(); }
    static Result failure(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool has_value() const {
        return std::holds_alternative<std::monostate>(value_);
    }
    [[nodiscard]] explicit operator bool() const { return has_value(); }
    [[nodiscard]] const Error& error() const { return std::get<Error>(value_); }
    [[nodiscard]] Error& error() { return std::get<Error>(value_); }

private:
    Result() = default;
    explicit Result(Error error) : value_(std::move(error)) {}

    std::variant<std::monostate, Error> value_;
};

}  // namespace zed::core

/*
 * Internal result value model.
 *
 * Result<T> carries exactly one of a success value or a Failure. Misusing an
 * accessor is an internal programming error (std::logic_error) and can never
 * cross the C ABI: the public entry points convert every internal failure to
 * stable status/error data before returning, and the camera contract consumes
 * Result values without throwing.
 */

#ifndef CVFORWIN_SRC_CORE_RESULT_H_
#define CVFORWIN_SRC_CORE_RESULT_H_

#include <optional>
#include <stdexcept>
#include <utility>

#include "core/error.h"

namespace cvforwin::core {

/*
 * Value-or-failure pair. Constructing from a T means success; constructing
 * from a Failure means failure. T must be move-constructible.
 */
template <typename T>
class Result {
public:
    Result(T value)
        : value_(std::move(value))
    {
    }

    Result(Failure failure)
        : failure_(std::move(failure))
    {
    }

    bool has_value() const noexcept
    {
        return value_.has_value();
    }

    explicit operator bool() const noexcept
    {
        return has_value();
    }

    T& value() &
    {
        if (!value_.has_value()) {
            throw std::logic_error("cvforwin::core::Result::value() called on a failure result");
        }
        return *value_;
    }

    const T& value() const&
    {
        if (!value_.has_value()) {
            throw std::logic_error("cvforwin::core::Result::value() called on a failure result");
        }
        return *value_;
    }

    T&& value() &&
    {
        if (!value_.has_value()) {
            throw std::logic_error("cvforwin::core::Result::value() called on a failure result");
        }
        return std::move(*value_);
    }

    const Failure& failure() const&
    {
        if (!failure_.has_value()) {
            throw std::logic_error("cvforwin::core::Result::failure() called on a valued result");
        }
        return *failure_;
    }

private:
    std::optional<T> value_;
    std::optional<Failure> failure_;
};

/* Void specialization: default construction means success. */
template <>
class Result<void> {
public:
    Result() noexcept = default;

    Result(Failure failure)
        : failure_(std::move(failure))
    {
    }

    bool has_value() const noexcept
    {
        return !failure_.has_value();
    }

    explicit operator bool() const noexcept
    {
        return has_value();
    }

    const Failure& failure() const&
    {
        if (!failure_.has_value()) {
            throw std::logic_error("cvforwin::core::Result<void>::failure() called on a success result");
        }
        return *failure_;
    }

private:
    std::optional<Failure> failure_;
};

}  // namespace cvforwin::core

#endif /* CVFORWIN_SRC_CORE_RESULT_H_ */

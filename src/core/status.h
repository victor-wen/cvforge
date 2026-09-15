/*
 * Internal status, verdict, and log-level model.
 *
 * The numeric values mirror the frozen public v1 ABI exactly, but this module
 * deliberately does not include or depend on the public C header so that
 * core_model stays ABI-independent. The public mapping is explicit in
 * status.cpp and verified by the developer unit tests.
 */

#ifndef CVFORWIN_SRC_CORE_STATUS_H_
#define CVFORWIN_SRC_CORE_STATUS_H_

#include <cstdint>
#include <optional>
#include <string_view>

namespace cvforwin::core {

enum class Status : std::uint32_t {
    ok = 0,
    invalid_argument = 1,
    abi_mismatch = 2,
    context_limit = 3,
    invalid_context = 4,
    config_error = 5,
    recipe_not_found = 6,
    camera_not_found = 7,
    camera_io = 8,
    timeout = 9,
    buffer_too_small = 10,
    algorithm_error = 11,
    required_artifact_error = 12,
    internal_error = 13,
};

enum class Verdict : std::uint32_t {
    not_evaluated = 0,
    pass = 1,
    fail = 2,
};

enum class LogLevel : std::uint32_t {
    trace = 0,
    debug = 1,
    info = 2,
    warn = 3,
    error = 4,
    critical = 5,
};

constexpr std::uint32_t to_public_status(Status status) noexcept
{
    return static_cast<std::uint32_t>(status);
}

constexpr std::uint32_t to_public_verdict(Verdict verdict) noexcept
{
    return static_cast<std::uint32_t>(verdict);
}

constexpr std::uint32_t to_public_log_level(LogLevel level) noexcept
{
    return static_cast<std::uint32_t>(level);
}

/* Returns std::nullopt for a value outside the frozen v1 status range. */
std::optional<Status> status_from_public(std::uint32_t value) noexcept;

/* Returns std::nullopt for a value outside the frozen v1 verdict range. */
std::optional<Verdict> verdict_from_public(std::uint32_t value) noexcept;

/* Returns std::nullopt for a value outside the frozen v1 log-level range. */
std::optional<LogLevel> log_level_from_public(std::uint32_t value) noexcept;

std::string_view status_name(Status status) noexcept;
std::string_view verdict_name(Verdict verdict) noexcept;
std::string_view log_level_name(LogLevel level) noexcept;

}  // namespace cvforwin::core

#endif /* CVFORWIN_SRC_CORE_STATUS_H_ */

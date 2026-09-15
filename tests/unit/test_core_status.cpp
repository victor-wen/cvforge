/*
 * Developer unit tests for the internal status/verdict/error-code model.
 *
 * These tests pin the internal mapping to the frozen v1 numeric values. The
 * public mapping itself is exercised independently by tests/abi.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>

#include "core/error.h"
#include "core/status.h"

using cvforwin::core::ErrorCode;
using cvforwin::core::LogLevel;
using cvforwin::core::Status;
using cvforwin::core::Verdict;

namespace {

constexpr std::array<Status, 14> kAllStatuses = {
    Status::ok,
    Status::invalid_argument,
    Status::abi_mismatch,
    Status::context_limit,
    Status::invalid_context,
    Status::config_error,
    Status::recipe_not_found,
    Status::camera_not_found,
    Status::camera_io,
    Status::timeout,
    Status::buffer_too_small,
    Status::algorithm_error,
    Status::required_artifact_error,
    Status::internal_error,
};

constexpr std::array<Verdict, 3> kAllVerdicts = {
    Verdict::not_evaluated,
    Verdict::pass,
    Verdict::fail,
};

constexpr std::array<LogLevel, 6> kAllLogLevels = {
    LogLevel::trace,
    LogLevel::debug,
    LogLevel::info,
    LogLevel::warn,
    LogLevel::error,
    LogLevel::critical,
};

constexpr std::array<ErrorCode, 24> kAllErrorCodes = {
    ErrorCode::none,
    ErrorCode::invalid_pointer,
    ErrorCode::out_context_null,
    ErrorCode::error_info_null,
    ErrorCode::error_info_invalid,
    ErrorCode::struct_size_mismatch,
    ErrorCode::abi_version_mismatch,
    ErrorCode::reserved_not_zero,
    ErrorCode::unknown_flags,
    ErrorCode::missing_log_callback,
    ErrorCode::text_argument_invalid,
    ErrorCode::text_argument_too_long,
    ErrorCode::path_not_absolute,
    ErrorCode::buffer_argument_invalid,
    ErrorCode::request_invalid,
    ErrorCode::context_limit_reached,
    ErrorCode::context_invalid,
    ErrorCode::config_root_missing,
    ErrorCode::config_root_unreadable,
    ErrorCode::subsystem_unavailable,
    ErrorCode::recipes_unavailable,
    ErrorCode::inspection_unavailable,
    ErrorCode::internal_exception,
    ErrorCode::internal_unexpected,
};

}  // namespace

TEST_CASE("status values match the frozen v1 numbering", "[core][status]")
{
    CHECK(cvforwin::core::to_public_status(Status::ok) == 0u);
    CHECK(cvforwin::core::to_public_status(Status::invalid_argument) == 1u);
    CHECK(cvforwin::core::to_public_status(Status::abi_mismatch) == 2u);
    CHECK(cvforwin::core::to_public_status(Status::context_limit) == 3u);
    CHECK(cvforwin::core::to_public_status(Status::invalid_context) == 4u);
    CHECK(cvforwin::core::to_public_status(Status::config_error) == 5u);
    CHECK(cvforwin::core::to_public_status(Status::recipe_not_found) == 6u);
    CHECK(cvforwin::core::to_public_status(Status::camera_not_found) == 7u);
    CHECK(cvforwin::core::to_public_status(Status::camera_io) == 8u);
    CHECK(cvforwin::core::to_public_status(Status::timeout) == 9u);
    CHECK(cvforwin::core::to_public_status(Status::buffer_too_small) == 10u);
    CHECK(cvforwin::core::to_public_status(Status::algorithm_error) == 11u);
    CHECK(cvforwin::core::to_public_status(Status::required_artifact_error) == 12u);
    CHECK(cvforwin::core::to_public_status(Status::internal_error) == 13u);
}

TEST_CASE("status values round-trip through the public numbering", "[core][status]")
{
    for (const Status status : kAllStatuses) {
        const auto round_trip = cvforwin::core::status_from_public(cvforwin::core::to_public_status(status));
        REQUIRE(round_trip.has_value());
        CHECK(round_trip.value() == status);
    }
    CHECK_FALSE(cvforwin::core::status_from_public(14u).has_value());
    CHECK_FALSE(cvforwin::core::status_from_public(0xFFFFFFFFu).has_value());
}

TEST_CASE("verdict values match the frozen v1 numbering", "[core][verdict]")
{
    CHECK(cvforwin::core::to_public_verdict(Verdict::not_evaluated) == 0u);
    CHECK(cvforwin::core::to_public_verdict(Verdict::pass) == 1u);
    CHECK(cvforwin::core::to_public_verdict(Verdict::fail) == 2u);
    for (const Verdict verdict : kAllVerdicts) {
        const auto round_trip = cvforwin::core::verdict_from_public(cvforwin::core::to_public_verdict(verdict));
        REQUIRE(round_trip.has_value());
        CHECK(round_trip.value() == verdict);
    }
    CHECK_FALSE(cvforwin::core::verdict_from_public(3u).has_value());
}

TEST_CASE("log levels match the frozen v1 numbering", "[core][logging]")
{
    for (std::size_t index = 0u; index < kAllLogLevels.size(); ++index) {
        CHECK(cvforwin::core::to_public_log_level(kAllLogLevels[index]) == static_cast<std::uint32_t>(index));
        const auto round_trip =
            cvforwin::core::log_level_from_public(cvforwin::core::to_public_log_level(kAllLogLevels[index]));
        REQUIRE(round_trip.has_value());
        CHECK(round_trip.value() == kAllLogLevels[index]);
    }
    CHECK_FALSE(cvforwin::core::log_level_from_public(6u).has_value());
}

TEST_CASE("status and verdict names are stable non-empty identifiers", "[core][status]")
{
    for (const Status status : kAllStatuses) {
        CHECK_FALSE(cvforwin::core::status_name(status).empty());
        CHECK(cvforwin::core::status_name(status).find("unknown") == std::string_view::npos);
    }
    for (const Verdict verdict : kAllVerdicts) {
        CHECK_FALSE(cvforwin::core::verdict_name(verdict).empty());
    }
    for (const LogLevel level : kAllLogLevels) {
        CHECK_FALSE(cvforwin::core::log_level_name(level).empty());
    }
}

TEST_CASE("error codes are nonzero and map to a non-OK status", "[core][error]")
{
    for (const ErrorCode code : kAllErrorCodes) {
        const std::uint32_t value = cvforwin::core::to_public_error_code(code);
        CHECK_FALSE(cvforwin::core::error_code_name(code).empty());
        if (code == ErrorCode::none) {
            CHECK(value == 0u);
            CHECK(cvforwin::core::status_for(code) == Status::ok);
        } else {
            CHECK(value != 0u);
            CHECK(cvforwin::core::status_for(code) != Status::ok);
        }
    }
}

TEST_CASE("error codes map to the documented broad statuses", "[core][error]")
{
    CHECK(cvforwin::core::status_for(ErrorCode::struct_size_mismatch) == Status::invalid_argument);
    CHECK(cvforwin::core::status_for(ErrorCode::abi_version_mismatch) == Status::abi_mismatch);
    CHECK(cvforwin::core::status_for(ErrorCode::context_limit_reached) == Status::context_limit);
    CHECK(cvforwin::core::status_for(ErrorCode::context_invalid) == Status::invalid_context);
    CHECK(cvforwin::core::status_for(ErrorCode::config_root_missing) == Status::config_error);
    CHECK(cvforwin::core::status_for(ErrorCode::internal_exception) == Status::internal_error);
}

TEST_CASE("make_failure keeps status, code, and message together", "[core][error]")
{
    const auto failure = cvforwin::core::make_failure(Status::config_error, ErrorCode::config_root_missing,
                                                      "configuration root does not exist");
    CHECK(failure.status == Status::config_error);
    CHECK(failure.code == ErrorCode::config_root_missing);
    CHECK_FALSE(failure.message.empty());
}

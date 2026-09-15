#include "core/status.h"

namespace cvforwin::core {

std::optional<Status> status_from_public(std::uint32_t value) noexcept
{
    switch (value) {
    case 0u:
        return Status::ok;
    case 1u:
        return Status::invalid_argument;
    case 2u:
        return Status::abi_mismatch;
    case 3u:
        return Status::context_limit;
    case 4u:
        return Status::invalid_context;
    case 5u:
        return Status::config_error;
    case 6u:
        return Status::recipe_not_found;
    case 7u:
        return Status::camera_not_found;
    case 8u:
        return Status::camera_io;
    case 9u:
        return Status::timeout;
    case 10u:
        return Status::buffer_too_small;
    case 11u:
        return Status::algorithm_error;
    case 12u:
        return Status::required_artifact_error;
    case 13u:
        return Status::internal_error;
    default:
        return std::nullopt;
    }
}

std::optional<Verdict> verdict_from_public(std::uint32_t value) noexcept
{
    switch (value) {
    case 0u:
        return Verdict::not_evaluated;
    case 1u:
        return Verdict::pass;
    case 2u:
        return Verdict::fail;
    default:
        return std::nullopt;
    }
}

std::optional<LogLevel> log_level_from_public(std::uint32_t value) noexcept
{
    switch (value) {
    case 0u:
        return LogLevel::trace;
    case 1u:
        return LogLevel::debug;
    case 2u:
        return LogLevel::info;
    case 3u:
        return LogLevel::warn;
    case 4u:
        return LogLevel::error;
    case 5u:
        return LogLevel::critical;
    default:
        return std::nullopt;
    }
}

std::string_view status_name(Status status) noexcept
{
    switch (status) {
    case Status::ok:
        return "CVF_STATUS_OK";
    case Status::invalid_argument:
        return "CVF_STATUS_INVALID_ARGUMENT";
    case Status::abi_mismatch:
        return "CVF_STATUS_ABI_MISMATCH";
    case Status::context_limit:
        return "CVF_STATUS_CONTEXT_LIMIT";
    case Status::invalid_context:
        return "CVF_STATUS_INVALID_CONTEXT";
    case Status::config_error:
        return "CVF_STATUS_CONFIG_ERROR";
    case Status::recipe_not_found:
        return "CVF_STATUS_RECIPE_NOT_FOUND";
    case Status::camera_not_found:
        return "CVF_STATUS_CAMERA_NOT_FOUND";
    case Status::camera_io:
        return "CVF_STATUS_CAMERA_IO";
    case Status::timeout:
        return "CVF_STATUS_TIMEOUT";
    case Status::buffer_too_small:
        return "CVF_STATUS_BUFFER_TOO_SMALL";
    case Status::algorithm_error:
        return "CVF_STATUS_ALGORITHM_ERROR";
    case Status::required_artifact_error:
        return "CVF_STATUS_REQUIRED_ARTIFACT_ERROR";
    case Status::internal_error:
        return "CVF_STATUS_INTERNAL_ERROR";
    }
    return "CVF_STATUS_<unknown>";
}

std::string_view verdict_name(Verdict verdict) noexcept
{
    switch (verdict) {
    case Verdict::not_evaluated:
        return "CVF_VERDICT_NOT_EVALUATED";
    case Verdict::pass:
        return "CVF_VERDICT_PASS";
    case Verdict::fail:
        return "CVF_VERDICT_FAIL";
    }
    return "CVF_VERDICT_<unknown>";
}

std::string_view log_level_name(LogLevel level) noexcept
{
    switch (level) {
    case LogLevel::trace:
        return "CVF_LOG_LEVEL_TRACE";
    case LogLevel::debug:
        return "CVF_LOG_LEVEL_DEBUG";
    case LogLevel::info:
        return "CVF_LOG_LEVEL_INFO";
    case LogLevel::warn:
        return "CVF_LOG_LEVEL_WARN";
    case LogLevel::error:
        return "CVF_LOG_LEVEL_ERROR";
    case LogLevel::critical:
        return "CVF_LOG_LEVEL_CRITICAL";
    }
    return "CVF_LOG_LEVEL_<unknown>";
}

}  // namespace cvforwin::core

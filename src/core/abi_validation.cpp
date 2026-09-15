#include "core/abi_validation.h"

#include <cstddef>

#include "core/text.h"

namespace cvforwin::core {
namespace {

template <std::size_t N>
bool reserved_is_zero(const std::array<std::uint32_t, N>& reserved) noexcept
{
    for (const std::uint32_t value : reserved) {
        if (value != 0u) {
            return false;
        }
    }
    return true;
}

std::optional<Failure> require_identifier(std::string_view text, std::uint32_t max_bytes, std::string_view field_name)
{
    if (text.empty()) {
        return invalid_argument(ErrorCode::text_argument_invalid, std::string(field_name) + " must not be empty");
    }
    const TextValidation validation = validate_text(text, max_bytes);
    if (!validation.ok) {
        switch (validation.problem) {
        case TextProblem::embedded_nul:
            return invalid_argument(ErrorCode::text_argument_invalid,
                                    std::string(field_name) + " must not contain an embedded NUL");
        case TextProblem::invalid_utf8:
            return invalid_argument(ErrorCode::text_argument_invalid,
                                    std::string(field_name) + " must be valid UTF-8");
        case TextProblem::too_long:
            return invalid_argument(ErrorCode::text_argument_too_long,
                                    std::string(field_name) + " exceeds its v1 byte limit");
        case TextProblem::null_pointer_with_capacity:
        case TextProblem::none:
            break;
        }
    }
    return std::nullopt;
}

std::optional<Failure> validate_root_path(std::string_view path, std::string_view field_name)
{
    if (path.empty()) {
        return invalid_argument(ErrorCode::text_argument_invalid,
                                std::string(field_name) + " must not be empty");
    }
    const TextValidation validation = validate_text(path, kMaxRootPathUtf8Bytes);
    if (!validation.ok) {
        return invalid_argument(ErrorCode::text_argument_invalid,
                                std::string(field_name) + " must be a bounded UTF-8 path");
    }
    if (!is_absolute_path(path)) {
        return invalid_argument(ErrorCode::path_not_absolute, std::string(field_name) + " must be an absolute path");
    }
    return std::nullopt;
}

std::optional<Failure> validate_caller_buffer(std::string_view field_name, bool present, std::uint32_t capacity,
                                              std::uint32_t required_capacity, bool required)
{
    if (!present) {
        if (capacity != 0u) {
            return invalid_argument(ErrorCode::buffer_argument_invalid,
                                    std::string(field_name) + " is NULL with a nonzero capacity");
        }
        if (!required) {
            return std::nullopt;
        }
        return invalid_argument(ErrorCode::buffer_argument_invalid,
                                std::string(field_name) + " is required and must not be NULL");
    }
    if (required && capacity < required_capacity) {
        return invalid_argument(ErrorCode::buffer_argument_invalid,
                                std::string(field_name) + " is smaller than its v1 required capacity");
    }
    if (!required && capacity > 0u && capacity < required_capacity) {
        return invalid_argument(ErrorCode::buffer_argument_invalid,
                                std::string(field_name) + " is smaller than its v1 required capacity");
    }
    return std::nullopt;
}

}  // namespace

std::optional<Failure> validate_error_info(const ErrorInfoView& error, std::size_t expected_struct_size)
{
    if (static_cast<std::size_t>(error.struct_size) != expected_struct_size) {
        return invalid_argument(ErrorCode::error_info_invalid,
                                "cvf_error_info_v1.struct_size does not match the v1 size");
    }
    if (!reserved_is_zero(error.reserved)) {
        return invalid_argument(ErrorCode::reserved_not_zero, "cvf_error_info_v1.reserved must be zero");
    }
    if (error.message_capacity > 0u && !error.message_present) {
        return invalid_argument(ErrorCode::buffer_argument_invalid,
                                "cvf_error_info_v1.message_utf8 is NULL with a nonzero capacity");
    }
    return std::nullopt;
}

std::optional<Failure> validate_init_options(const InitOptionsView& options, std::size_t expected_struct_size)
{
    if (static_cast<std::size_t>(options.struct_size) != expected_struct_size) {
        return invalid_argument(ErrorCode::struct_size_mismatch,
                                "cvf_init_options_v1.struct_size does not match the v1 size");
    }
    if (options.abi_version != kAbiVersionV1) {
        return make_failure(Status::abi_mismatch, ErrorCode::abi_version_mismatch,
                            "cvf_init_options_v1.abi_version must equal CVF_ABI_VERSION_V1");
    }
    if (!reserved_is_zero(options.reserved)) {
        return invalid_argument(ErrorCode::reserved_not_zero, "cvf_init_options_v1.reserved must be zero");
    }
    if ((options.flags & ~kKnownInitFlagsMask) != 0u) {
        return invalid_argument(ErrorCode::unknown_flags, "cvf_init_options_v1.flags contains unknown bits");
    }
    if ((options.flags & kInitFlagCallbackLogging) != 0u && !options.log_callback_present) {
        return invalid_argument(ErrorCode::missing_log_callback,
                                "CVF_INIT_FLAG_CALLBACK_LOGGING requires a non-NULL log_callback");
    }
    if (auto failure = validate_root_path(options.config_root, "config_root_utf8"); failure.has_value()) {
        return failure;
    }
    return validate_root_path(options.output_root, "output_root_utf8");
}

std::optional<Failure> validate_inspection_request(const InspectionRequestView& request, std::size_t expected_struct_size)
{
    if (static_cast<std::size_t>(request.struct_size) != expected_struct_size) {
        return invalid_argument(ErrorCode::struct_size_mismatch,
                                "cvf_inspection_request_v1.struct_size does not match the v1 size");
    }
    if (request.abi_version != kAbiVersionV1) {
        return make_failure(Status::abi_mismatch, ErrorCode::abi_version_mismatch,
                            "cvf_inspection_request_v1.abi_version must equal CVF_ABI_VERSION_V1");
    }
    if (!reserved_is_zero(request.reserved)) {
        return invalid_argument(ErrorCode::reserved_not_zero, "cvf_inspection_request_v1.reserved must be zero");
    }
    if (auto failure = require_identifier(request.recipe_id, kMaxRecipeIdBytes, "recipe_id_utf8");
        failure.has_value()) {
        return failure;
    }
    if (auto failure = require_identifier(request.request_id, kMaxRequestIdBytes, "request_id_utf8");
        failure.has_value()) {
        return failure;
    }
    if (request.input_json_present) {
        const TextValidation validation = validate_text(request.input_json, kMaxInputJsonBytes);
        if (!validation.ok) {
            switch (validation.problem) {
            case TextProblem::too_long:
                return invalid_argument(ErrorCode::text_argument_too_long,
                                        "input_json_utf8 exceeds its v1 byte limit");
            case TextProblem::embedded_nul:
            case TextProblem::invalid_utf8:
                return invalid_argument(ErrorCode::text_argument_invalid,
                                        "input_json_utf8 must be valid UTF-8 without an embedded NUL");
            case TextProblem::null_pointer_with_capacity:
            case TextProblem::none:
                break;
            }
        }
    }
    return std::nullopt;
}

std::optional<Failure> validate_inspection_result(const InspectionResultView& result, std::size_t expected_struct_size)
{
    if (static_cast<std::size_t>(result.struct_size) != expected_struct_size) {
        return invalid_argument(ErrorCode::struct_size_mismatch,
                                "cvf_inspection_result_v1.struct_size does not match the v1 size");
    }
    if (!reserved_is_zero(result.reserved)) {
        return invalid_argument(ErrorCode::reserved_not_zero, "cvf_inspection_result_v1.reserved must be zero");
    }
    if (auto failure =
            validate_caller_buffer("result.output_json", result.output_json_present, result.output_json_capacity,
                                   kRequiredResultJsonCapacity, true);
        failure.has_value()) {
        return failure;
    }
    if (auto failure =
            validate_caller_buffer("result.image_path", result.image_path_present, result.image_path_capacity,
                                   kRequiredImagePathCapacity, true);
        failure.has_value()) {
        return failure;
    }
    return validate_caller_buffer("result.error_message", result.error_message_present, result.error_message_capacity,
                                  kRequiredErrorMessageCapacity, false);
}

}  // namespace cvforwin::core

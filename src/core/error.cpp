#include "core/error.h"

#include <utility>

namespace cvforwin::core {

Status status_for(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::none:
        return Status::ok;

    case ErrorCode::invalid_pointer:
    case ErrorCode::out_context_null:
    case ErrorCode::error_info_null:
    case ErrorCode::error_info_invalid:
    case ErrorCode::struct_size_mismatch:
    case ErrorCode::reserved_not_zero:
    case ErrorCode::unknown_flags:
    case ErrorCode::missing_log_callback:
    case ErrorCode::text_argument_invalid:
    case ErrorCode::text_argument_too_long:
    case ErrorCode::path_not_absolute:
    case ErrorCode::buffer_argument_invalid:
    case ErrorCode::request_invalid:
    case ErrorCode::diagnostics_invalid_config:
        return Status::invalid_argument;

    case ErrorCode::abi_version_mismatch:
        return Status::abi_mismatch;

    case ErrorCode::context_limit_reached:
        return Status::context_limit;

    case ErrorCode::context_invalid:
    case ErrorCode::runtime_context_closed:
        return Status::invalid_context;

    case ErrorCode::config_root_missing:
    case ErrorCode::config_root_unreadable:
    case ErrorCode::subsystem_unavailable:
    case ErrorCode::recipes_unavailable:
    case ErrorCode::inspection_unavailable:
    case ErrorCode::config_file_missing:
    case ErrorCode::config_parse_error:
    case ErrorCode::config_duplicate_key:
    case ErrorCode::config_unknown_key:
    case ErrorCode::config_value_invalid:
    case ErrorCode::config_schema_version:
    case ErrorCode::recipes_dir_missing:
    case ErrorCode::config_io_error:
    case ErrorCode::recipe_file_missing:
    case ErrorCode::recipe_parse_error:
    case ErrorCode::recipe_duplicate_key:
    case ErrorCode::recipe_unknown_key:
    case ErrorCode::recipe_value_invalid:
    case ErrorCode::recipe_schema_version:
    case ErrorCode::recipe_id_invalid:
    case ErrorCode::recipe_id_duplicate:
    case ErrorCode::recipe_algorithm_unknown:
    case ErrorCode::recipe_parameters_invalid:
    case ErrorCode::recipe_capture_invalid:
    case ErrorCode::recipe_artifacts_invalid:
    case ErrorCode::recipe_io_error:
    case ErrorCode::artifacts_root_error:
    case ErrorCode::runtime_reload_failed:
        return Status::config_error;

    case ErrorCode::recipe_not_found:
        return Status::recipe_not_found;

    case ErrorCode::internal_exception:
    case ErrorCode::internal_unexpected:
    case ErrorCode::log_write_error:
    case ErrorCode::image_encode_error:
    case ErrorCode::image_write_error:
    case ErrorCode::retention_error:
        return Status::internal_error;

    case ErrorCode::camera_not_found:
    case ErrorCode::camera_identity_ambiguous:
    case ErrorCode::descriptor_mismatch:
        return Status::camera_not_found;

    case ErrorCode::camera_not_open:
    case ErrorCode::camera_disconnected:
    case ErrorCode::capture_failed:
    case ErrorCode::frames_exhausted:
        return Status::camera_io;

    case ErrorCode::capture_timed_out:
    case ErrorCode::deadline_expired:
    case ErrorCode::runtime_queue_timeout:
        return Status::timeout;

    case ErrorCode::runtime_result_too_large:
        return Status::buffer_too_small;

    case ErrorCode::runtime_required_artifact_failed:
        return Status::required_artifact_error;

    case ErrorCode::selector_empty:
    case ErrorCode::algorithm_key_invalid:
    case ErrorCode::algorithm_input_invalid:
        return Status::invalid_argument;

    case ErrorCode::algorithm_parameters_invalid:
        return Status::config_error;

    case ErrorCode::algorithm_deadline_exceeded:
        return Status::timeout;

    case ErrorCode::algorithm_not_found:
    case ErrorCode::algorithm_duplicate_key:
    case ErrorCode::algorithm_output_too_large:
    case ErrorCode::algorithm_exception:
    case ErrorCode::algorithm_frame_invalid:
        return Status::algorithm_error;
    }
    return Status::internal_error;
}

std::string_view error_code_name(ErrorCode code) noexcept
{
    switch (code) {
    case ErrorCode::none:
        return "none";
    case ErrorCode::invalid_pointer:
        return "invalid_pointer";
    case ErrorCode::out_context_null:
        return "out_context_null";
    case ErrorCode::error_info_null:
        return "error_info_null";
    case ErrorCode::error_info_invalid:
        return "error_info_invalid";
    case ErrorCode::struct_size_mismatch:
        return "struct_size_mismatch";
    case ErrorCode::abi_version_mismatch:
        return "abi_version_mismatch";
    case ErrorCode::reserved_not_zero:
        return "reserved_not_zero";
    case ErrorCode::unknown_flags:
        return "unknown_flags";
    case ErrorCode::missing_log_callback:
        return "missing_log_callback";
    case ErrorCode::text_argument_invalid:
        return "text_argument_invalid";
    case ErrorCode::text_argument_too_long:
        return "text_argument_too_long";
    case ErrorCode::path_not_absolute:
        return "path_not_absolute";
    case ErrorCode::buffer_argument_invalid:
        return "buffer_argument_invalid";
    case ErrorCode::request_invalid:
        return "request_invalid";
    case ErrorCode::context_limit_reached:
        return "context_limit_reached";
    case ErrorCode::context_invalid:
        return "context_invalid";
    case ErrorCode::config_root_missing:
        return "config_root_missing";
    case ErrorCode::config_root_unreadable:
        return "config_root_unreadable";
    case ErrorCode::subsystem_unavailable:
        return "subsystem_unavailable";
    case ErrorCode::recipes_unavailable:
        return "recipes_unavailable";
    case ErrorCode::inspection_unavailable:
        return "inspection_unavailable";
    case ErrorCode::internal_exception:
        return "internal_exception";
    case ErrorCode::internal_unexpected:
        return "internal_unexpected";
    case ErrorCode::camera_not_found:
        return "camera_not_found";
    case ErrorCode::camera_identity_ambiguous:
        return "camera_identity_ambiguous";
    case ErrorCode::camera_not_open:
        return "camera_not_open";
    case ErrorCode::camera_disconnected:
        return "camera_disconnected";
    case ErrorCode::capture_failed:
        return "capture_failed";
    case ErrorCode::capture_timed_out:
        return "capture_timed_out";
    case ErrorCode::frames_exhausted:
        return "frames_exhausted";
    case ErrorCode::deadline_expired:
        return "deadline_expired";
    case ErrorCode::selector_empty:
        return "selector_empty";
    case ErrorCode::descriptor_mismatch:
        return "descriptor_mismatch";
    case ErrorCode::algorithm_not_found:
        return "algorithm_not_found";
    case ErrorCode::algorithm_duplicate_key:
        return "algorithm_duplicate_key";
    case ErrorCode::algorithm_key_invalid:
        return "algorithm_key_invalid";
    case ErrorCode::algorithm_parameters_invalid:
        return "algorithm_parameters_invalid";
    case ErrorCode::algorithm_output_too_large:
        return "algorithm_output_too_large";
    case ErrorCode::algorithm_deadline_exceeded:
        return "algorithm_deadline_exceeded";
    case ErrorCode::algorithm_exception:
        return "algorithm_exception";
    case ErrorCode::algorithm_frame_invalid:
        return "algorithm_frame_invalid";
    case ErrorCode::algorithm_input_invalid:
        return "algorithm_input_invalid";
    case ErrorCode::config_file_missing:
        return "config_file_missing";
    case ErrorCode::config_parse_error:
        return "config_parse_error";
    case ErrorCode::config_duplicate_key:
        return "config_duplicate_key";
    case ErrorCode::config_unknown_key:
        return "config_unknown_key";
    case ErrorCode::config_value_invalid:
        return "config_value_invalid";
    case ErrorCode::config_schema_version:
        return "config_schema_version";
    case ErrorCode::recipes_dir_missing:
        return "recipes_dir_missing";
    case ErrorCode::config_io_error:
        return "config_io_error";
    case ErrorCode::recipe_file_missing:
        return "recipe_file_missing";
    case ErrorCode::recipe_parse_error:
        return "recipe_parse_error";
    case ErrorCode::recipe_duplicate_key:
        return "recipe_duplicate_key";
    case ErrorCode::recipe_unknown_key:
        return "recipe_unknown_key";
    case ErrorCode::recipe_value_invalid:
        return "recipe_value_invalid";
    case ErrorCode::recipe_schema_version:
        return "recipe_schema_version";
    case ErrorCode::recipe_id_invalid:
        return "recipe_id_invalid";
    case ErrorCode::recipe_id_duplicate:
        return "recipe_id_duplicate";
    case ErrorCode::recipe_algorithm_unknown:
        return "recipe_algorithm_unknown";
    case ErrorCode::recipe_parameters_invalid:
        return "recipe_parameters_invalid";
    case ErrorCode::recipe_capture_invalid:
        return "recipe_capture_invalid";
    case ErrorCode::recipe_artifacts_invalid:
        return "recipe_artifacts_invalid";
    case ErrorCode::recipe_not_found:
        return "recipe_not_found";
    case ErrorCode::recipe_io_error:
        return "recipe_io_error";
    case ErrorCode::diagnostics_invalid_config:
        return "diagnostics_invalid_config";
    case ErrorCode::log_write_error:
        return "log_write_error";
    case ErrorCode::artifacts_root_error:
        return "artifacts_root_error";
    case ErrorCode::image_encode_error:
        return "image_encode_error";
    case ErrorCode::image_write_error:
        return "image_write_error";
    case ErrorCode::retention_error:
        return "retention_error";
    case ErrorCode::runtime_queue_timeout:
        return "runtime_queue_timeout";
    case ErrorCode::runtime_result_too_large:
        return "runtime_result_too_large";
    case ErrorCode::runtime_required_artifact_failed:
        return "runtime_required_artifact_failed";
    case ErrorCode::runtime_reload_failed:
        return "runtime_reload_failed";
    case ErrorCode::runtime_context_closed:
        return "runtime_context_closed";
    }
    return "unrecognized_error_code";
}

Failure make_failure(Status status, ErrorCode code, std::string message)
{
    Failure failure;
    failure.status = status;
    failure.code = code;
    failure.message = std::move(message);
    return failure;
}

Failure invalid_argument(ErrorCode code, std::string message)
{
    return make_failure(Status::invalid_argument, code, std::move(message));
}

}  // namespace cvforwin::core

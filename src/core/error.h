/*
 * Internal failure model.
 *
 * A Failure couples the broad public status with a stable subsystem-specific
 * error code and a bounded, ASCII-safe diagnostic message. Error codes are
 * nonzero for every failure; ErrorCode::none is the success value and must
 * never be attached to a non-OK status.
 */

#ifndef CVFORWIN_SRC_CORE_ERROR_H_
#define CVFORWIN_SRC_CORE_ERROR_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "core/status.h"

namespace cvforwin::core {

enum class ErrorCode : std::uint32_t {
    none = 0,

    invalid_pointer = 1000,
    out_context_null = 1001,
    error_info_null = 1002,
    error_info_invalid = 1003,
    struct_size_mismatch = 1004,
    abi_version_mismatch = 1005,
    reserved_not_zero = 1006,
    unknown_flags = 1007,
    missing_log_callback = 1008,
    text_argument_invalid = 1009,
    text_argument_too_long = 1010,
    path_not_absolute = 1011,
    buffer_argument_invalid = 1012,
    request_invalid = 1013,

    context_limit_reached = 1100,
    context_invalid = 1101,

    config_root_missing = 1200,
    config_root_unreadable = 1201,
    subsystem_unavailable = 1202,
    recipes_unavailable = 1203,
    inspection_unavailable = 1204,

    internal_exception = 1300,
    internal_unexpected = 1301,

    camera_not_found = 1400,
    camera_identity_ambiguous = 1401,
    camera_not_open = 1402,
    camera_disconnected = 1403,
    capture_failed = 1404,
    capture_timed_out = 1405,
    frames_exhausted = 1406,
    deadline_expired = 1407,
    selector_empty = 1408,
    descriptor_mismatch = 1409,

    algorithm_not_found = 1500,
    algorithm_duplicate_key = 1501,
    algorithm_key_invalid = 1502,
    algorithm_parameters_invalid = 1503,
    algorithm_output_too_large = 1504,
    algorithm_deadline_exceeded = 1505,
    algorithm_exception = 1506,
    algorithm_frame_invalid = 1507,
    algorithm_input_invalid = 1508,

    /* recipe_store subsystem: global configuration (1600-1607) and recipes (1610-1623). */
    config_file_missing = 1600,
    config_parse_error = 1601,
    config_duplicate_key = 1602,
    config_unknown_key = 1603,
    config_value_invalid = 1604,
    config_schema_version = 1605,
    recipes_dir_missing = 1606,
    config_io_error = 1607,

    recipe_file_missing = 1610,
    recipe_parse_error = 1611,
    recipe_duplicate_key = 1612,
    recipe_unknown_key = 1613,
    recipe_value_invalid = 1614,
    recipe_schema_version = 1615,
    recipe_id_invalid = 1616,
    recipe_id_duplicate = 1617,
    recipe_algorithm_unknown = 1618,
    recipe_parameters_invalid = 1619,
    recipe_capture_invalid = 1620,
    recipe_artifacts_invalid = 1621,
    recipe_not_found = 1622,
    recipe_io_error = 1623,

    /* diagnostics_and_artifacts subsystem: diagnostics (1700-1701) and managed captures (1800-1803). */
    diagnostics_invalid_config = 1700,
    log_write_error = 1701,

    artifacts_root_error = 1800,
    image_encode_error = 1801,
    image_write_error = 1802,
    retention_error = 1803,

    /* runtime_orchestrator subsystem (1900-1904). */
    runtime_queue_timeout = 1900,
    runtime_result_too_large = 1901,
    runtime_required_artifact_failed = 1902,
    runtime_reload_failed = 1903,
    runtime_context_closed = 1904,
};

/* Broad public status a failure maps to. */
Status status_for(ErrorCode code) noexcept;

/* Stable symbolic name, used in diagnostics and tests. */
std::string_view error_code_name(ErrorCode code) noexcept;

constexpr std::uint32_t to_public_error_code(ErrorCode code) noexcept
{
    return static_cast<std::uint32_t>(code);
}

struct Failure {
    Status status = Status::internal_error;
    ErrorCode code = ErrorCode::internal_unexpected;
    std::string message;
};

Failure make_failure(Status status, ErrorCode code, std::string message);

/* Builds an invalid-argument failure with a stable message. */
Failure invalid_argument(ErrorCode code, std::string message);

}  // namespace cvforwin::core

#endif /* CVFORWIN_SRC_CORE_ERROR_H_ */

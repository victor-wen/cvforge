/*
 * Implementation of the frozen v1 public C ABI.
 *
 * Every exported entry point is a validating boundary:
 *   1. pointers and the error structure are validated without side effects,
 *   2. the context state is checked before any other work,
 *   3. request/result structures are mirrored into internal values and
 *      validated, including the v1 required buffer capacities,
 *   4. only then is the runtime orchestrator invoked.
 *
 * The runtime owns the camera session, recipe snapshot, diagnostics,
 * artifacts, and serialization gate. A failure inside an inspection is a
 * valued runtime outcome whose status/verdict/error_code/warning_flags land in
 * the caller result; a technical error never reports PASS/FAIL.
 *
 * No exception may cross the ABI: every entry point contains a catch-all that
 * converts a failure into CVF_STATUS_INTERNAL_ERROR with a stable error code.
 */

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <cvforwin/cvf_api.h>

#include "c_api/cvf_context.h"
#include "core/abi_validation.h"
#include "core/error.h"
#include "core/status.h"
#include "core/text.h"

namespace {

namespace core = cvforwin::core;
namespace capi = cvforwin::capi;
namespace runtime = cvforwin::runtime;
namespace diagnostics = cvforwin::diagnostics;

constexpr std::uint32_t kExpectedErrorInfoSize = static_cast<std::uint32_t>(sizeof(cvf_error_info_v1));
constexpr std::uint32_t kExpectedInitOptionsSize = static_cast<std::uint32_t>(sizeof(cvf_init_options_v1));
constexpr std::uint32_t kExpectedRequestSize = static_cast<std::uint32_t>(sizeof(cvf_inspection_request_v1));
constexpr std::uint32_t kExpectedResultSize = static_cast<std::uint32_t>(sizeof(cvf_inspection_result_v1));

/* Keeps diagnostics short so the required size always fits the v1 capacity. */
constexpr std::size_t kPathEchoLimit = 200u;

/*
 * True when the caller supplied a writable v1 error structure. When this is
 * false the DLL must not touch any field of the structure.
 */
bool error_info_is_writable(const cvf_error_info_v1* error)
{
    if (error == nullptr || error->struct_size != kExpectedErrorInfoSize) {
        return false;
    }
    if (error->message_capacity > 0u && error->message_utf8 == nullptr) {
        return false;
    }
    return true;
}

/* Writes failure data into a caller error structure following the v1 rules. */
void store_error(cvf_error_info_v1* error, const core::Failure& failure)
{
    if (!error_info_is_writable(error)) {
        return;
    }
    error->error_code = core::to_public_error_code(failure.code);
    const core::TextWriteResult written =
        core::write_text(error->message_utf8, error->message_capacity, failure.message);
    error->message_bytes_written = written.bytes_written;
    error->message_bytes_required = written.bytes_required;
}

void clear_error(cvf_error_info_v1* error)
{
    if (!error_info_is_writable(error)) {
        return;
    }
    error->error_code = 0u;
    error->message_bytes_written = 0u;
    error->message_bytes_required = 0u;
    if (error->message_utf8 != nullptr && error->message_capacity > 0u) {
        error->message_utf8[0] = '\0';
    }
}

bool reserved_is_zero(const cvf_inspection_result_v1* result)
{
    for (const std::uint32_t value : result->reserved) {
        if (value != 0u) {
            return false;
        }
    }
    return true;
}

/*
 * The single validity predicate for every write into a caller result
 * structure. struct_size and reserved are checked before any field is touched;
 * a malformed result is reported through the function return status only and
 * is never modified, whichever failure path is active.
 */
bool result_is_writable(const cvf_inspection_result_v1* result)
{
    if (result == nullptr || result->struct_size != kExpectedResultSize) {
        return false;
    }
    return reserved_is_zero(result);
}

/*
 * Clears a result text buffer that the failed call did not produce: the
 * counters report zero bytes written and nothing required, and the buffer is
 * kept NUL-terminated whenever writing one byte is safe.
 */
void clear_result_text(char* buffer, std::uint32_t capacity, std::uint32_t& bytes_written,
                       std::uint32_t& bytes_required)
{
    bytes_written = 0u;
    bytes_required = 0u;
    if (buffer != nullptr && capacity > 0u) {
        buffer[0] = '\0';
    }
}

/*
 * Reports a text field that cannot hold its v1 required capacity: nothing is
 * written and bytes_required reports the capacity the caller must provide.
 */
void mark_required_text(char* buffer, std::uint32_t capacity, std::uint32_t required_capacity,
                        std::uint32_t& bytes_written, std::uint32_t& bytes_required)
{
    bytes_written = 0u;
    bytes_required = required_capacity;
    if (buffer != nullptr && capacity > 0u) {
        buffer[0] = '\0';
    }
}

void write_result_text(char* buffer, std::uint32_t capacity, const std::string& text,
                       std::uint32_t& bytes_written, std::uint32_t& bytes_required)
{
    if (text.empty()) {
        clear_result_text(buffer, capacity, bytes_written, bytes_required);
        return;
    }
    const core::TextWriteResult written = core::write_text(buffer, capacity, text);
    bytes_written = written.bytes_written;
    bytes_required = written.bytes_required;
}

/* Which caller result buffer was too small for the pre-call capacity check. */
struct BufferShortfall {
    bool output_json = false;
    bool image_path = false;
    bool error_message = false;
};

/*
 * Publishes a non-OK inspection outcome into the caller result structure.
 *
 * Every write into the result goes through this one function and the single
 * result_is_writable predicate above, so a malformed structure is never partly
 * updated:
 *   - status mirrors the broad status returned by cvf_inspect,
 *   - verdict is always NOT_EVALUATED (a technical error is never PASS/FAIL),
 *   - error_code carries the stable detail code,
 *   - output_json and image_path are cleared; a buffer that was too small for
 *     its v1 required capacity reports that capacity in bytes_required,
 *   - error_message receives the bounded diagnostic when the caller supplied a
 *     usable buffer for it.
 */
void publish_result_failure(cvf_inspection_result_v1* result, const core::Failure& failure,
                            const BufferShortfall& shortfall = BufferShortfall{})
{
    if (!result_is_writable(result)) {
        return;
    }
    result->status = core::to_public_status(failure.status);
    result->verdict = core::to_public_verdict(core::Verdict::not_evaluated);
    result->error_code = core::to_public_error_code(failure.code);

    if (shortfall.output_json) {
        mark_required_text(result->output_json, result->output_json_capacity, CVF_RESULT_JSON_REQUIRED_CAPACITY,
                           result->output_json_bytes_written, result->output_json_bytes_required);
    } else {
        clear_result_text(result->output_json, result->output_json_capacity, result->output_json_bytes_written,
                          result->output_json_bytes_required);
    }
    if (shortfall.image_path) {
        mark_required_text(result->image_path, result->image_path_capacity, CVF_IMAGE_PATH_REQUIRED_CAPACITY,
                           result->image_path_bytes_written, result->image_path_bytes_required);
    } else {
        clear_result_text(result->image_path, result->image_path_capacity, result->image_path_bytes_written,
                          result->image_path_bytes_required);
    }
    if (shortfall.error_message) {
        mark_required_text(result->error_message, result->error_message_capacity,
                           CVF_ERROR_MESSAGE_REQUIRED_CAPACITY, result->error_message_bytes_written,
                           result->error_message_bytes_required);
    } else if (result->error_message != nullptr && result->error_message_capacity > 0u) {
        const core::TextWriteResult written =
            core::write_text(result->error_message, result->error_message_capacity, failure.message);
        result->error_message_bytes_written = written.bytes_written;
        result->error_message_bytes_required = written.bytes_required;
    } else {
        clear_result_text(result->error_message, result->error_message_capacity, result->error_message_bytes_written,
                          result->error_message_bytes_required);
    }
}

/*
 * Every mirror below checks struct_size before touching any other field, so a
 * caller that passes an older or truncated structure is never read past the
 * size it declared.
 */

std::optional<core::Failure> validate_error_struct(const cvf_error_info_v1* error)
{
    if (error->struct_size != kExpectedErrorInfoSize) {
        return core::invalid_argument(core::ErrorCode::error_info_invalid,
                                      "cvf_error_info_v1.struct_size does not match the v1 size");
    }
    core::ErrorInfoView view;
    view.struct_size = error->struct_size;
    view.message_present = error->message_utf8 != nullptr;
    view.message_capacity = error->message_capacity;
    view.reserved = {error->reserved[0], error->reserved[1], error->reserved[2], error->reserved[3]};
    return core::validate_error_info(view, static_cast<std::size_t>(kExpectedErrorInfoSize));
}

std::optional<core::Failure> validate_options_struct(const cvf_init_options_v1* options)
{
    if (options->struct_size != kExpectedInitOptionsSize) {
        return core::invalid_argument(core::ErrorCode::struct_size_mismatch,
                                      "cvf_init_options_v1.struct_size does not match the v1 size");
    }
    core::InitOptionsView view;
    view.struct_size = options->struct_size;
    view.abi_version = options->abi_version;
    if (options->config_root_utf8 != nullptr) {
        view.config_root = std::string_view(options->config_root_utf8, options->config_root_utf8_bytes);
    }
    if (options->output_root_utf8 != nullptr) {
        view.output_root = std::string_view(options->output_root_utf8, options->output_root_utf8_bytes);
    }
    view.flags = options->flags;
    view.log_callback_present = options->log_callback != nullptr;
    for (std::size_t index = 0u; index < view.reserved.size(); ++index) {
        view.reserved[index] = options->reserved[index];
    }
    return core::validate_init_options(view, static_cast<std::size_t>(kExpectedInitOptionsSize));
}

std::optional<core::Failure> validate_request_struct(const cvf_inspection_request_v1* request)
{
    if (request->struct_size != kExpectedRequestSize) {
        return core::invalid_argument(core::ErrorCode::struct_size_mismatch,
                                      "cvf_inspection_request_v1.struct_size does not match the v1 size");
    }
    /* Every pointer-length pair is checked before a view is constructed. A
     * required identifier needs a non-NULL pointer and a positive length; an
     * optional input_json needs a non-NULL pointer only when its length is
     * positive, and a zero length means absent regardless of the pointer. */
    if (request->recipe_id_utf8 == nullptr && request->recipe_id_utf8_bytes != 0u) {
        return core::invalid_argument(core::ErrorCode::buffer_argument_invalid,
                                      "recipe_id_utf8 is NULL with a nonzero length");
    }
    if (request->request_id_utf8 == nullptr && request->request_id_utf8_bytes != 0u) {
        return core::invalid_argument(core::ErrorCode::buffer_argument_invalid,
                                      "request_id_utf8 is NULL with a nonzero length");
    }
    if (request->input_json_utf8 == nullptr && request->input_json_utf8_bytes != 0u) {
        return core::invalid_argument(core::ErrorCode::buffer_argument_invalid,
                                      "input_json_utf8 is NULL with a nonzero length");
    }
    core::InspectionRequestView view;
    view.struct_size = request->struct_size;
    view.abi_version = request->abi_version;
    if (request->recipe_id_utf8 != nullptr) {
        view.recipe_id = std::string_view(request->recipe_id_utf8, request->recipe_id_utf8_bytes);
    }
    if (request->request_id_utf8 != nullptr) {
        view.request_id = std::string_view(request->request_id_utf8, request->request_id_utf8_bytes);
    }
    if (request->input_json_utf8 != nullptr && request->input_json_utf8_bytes != 0u) {
        view.input_json_present = true;
        view.input_json = std::string_view(request->input_json_utf8, request->input_json_utf8_bytes);
    }
    for (std::size_t index = 0u; index < view.reserved.size(); ++index) {
        view.reserved[index] = request->reserved[index];
    }
    return core::validate_inspection_request(view, static_cast<std::size_t>(kExpectedRequestSize));
}

/*
 * Required-capacity pre-check for cvf_inspect, run before any side effect and
 * before any runtime call. The v1 pointer rules are preserved: a NULL pointer
 * with a nonzero capacity is INVALID_ARGUMENT. A required buffer below its v1
 * capacity (or absent) is BUFFER_TOO_SMALL and the caller receives the
 * capacity it must provide through bytes_required.
 */
std::optional<std::pair<core::Failure, BufferShortfall>> result_capacity_problem(
    const cvf_inspection_result_v1* result)
{
    if (result->output_json == nullptr && result->output_json_capacity > 0u) {
        return std::make_pair(core::invalid_argument(core::ErrorCode::buffer_argument_invalid,
                                                     "result.output_json is NULL with a nonzero capacity"),
                              BufferShortfall{});
    }
    if (result->output_json_capacity < CVF_RESULT_JSON_REQUIRED_CAPACITY) {
        BufferShortfall shortfall;
        shortfall.output_json = true;
        return std::make_pair(core::make_failure(core::Status::buffer_too_small,
                                                 core::ErrorCode::buffer_argument_invalid,
                                                 "result.output_json is smaller than its v1 required capacity"),
                              shortfall);
    }
    if (result->image_path == nullptr && result->image_path_capacity > 0u) {
        return std::make_pair(core::invalid_argument(core::ErrorCode::buffer_argument_invalid,
                                                     "result.image_path is NULL with a nonzero capacity"),
                              BufferShortfall{});
    }
    if (result->image_path_capacity < CVF_IMAGE_PATH_REQUIRED_CAPACITY) {
        BufferShortfall shortfall;
        shortfall.image_path = true;
        return std::make_pair(core::make_failure(core::Status::buffer_too_small,
                                                 core::ErrorCode::buffer_argument_invalid,
                                                 "result.image_path is smaller than its v1 required capacity"),
                              shortfall);
    }
    if (result->error_message == nullptr && result->error_message_capacity > 0u) {
        return std::make_pair(core::invalid_argument(core::ErrorCode::buffer_argument_invalid,
                                                     "result.error_message is NULL with a nonzero capacity"),
                              BufferShortfall{});
    }
    if (result->error_message_capacity > 0u &&
        result->error_message_capacity < CVF_ERROR_MESSAGE_REQUIRED_CAPACITY) {
        BufferShortfall shortfall;
        shortfall.error_message = true;
        return std::make_pair(core::make_failure(core::Status::buffer_too_small,
                                                 core::ErrorCode::buffer_argument_invalid,
                                                 "result.error_message is smaller than its v1 required capacity"),
                              shortfall);
    }
    return std::nullopt;
}

/* Mirrors the validated request into the internal runtime value. */
runtime::InspectionRequest make_runtime_request(const cvf_inspection_request_v1* request)
{
    runtime::InspectionRequest runtime_request;
    runtime_request.recipe_id = std::string(request->recipe_id_utf8, request->recipe_id_utf8_bytes);
    runtime_request.request_id = std::string(request->request_id_utf8, request->request_id_utf8_bytes);
    runtime_request.timeout_ms = request->timeout_ms;
    if (request->input_json_utf8 != nullptr && request->input_json_utf8_bytes != 0u) {
        runtime_request.input_json = std::string(request->input_json_utf8, request->input_json_utf8_bytes);
    }
    return runtime_request;
}

cvf_status_t status_code(const core::Failure& failure) noexcept
{
    return core::to_public_status(failure.status);
}

/* ------------------------------------------------------------------ */
/* Export implementations                                              */
/* ------------------------------------------------------------------ */

std::uint32_t CVF_CALL get_abi_version_impl() noexcept
{
    return CVF_ABI_VERSION_V1;
}

cvf_status_t CVF_CALL initialize_impl(const cvf_init_options_v1* options, cvf_context** out_context,
                                      cvf_error_info_v1* error)
{
    if (out_context == nullptr) {
        return CVF_STATUS_INVALID_ARGUMENT;
    }
    /* The failure contract guarantees a NULL out_context for every rejection. */
    *out_context = nullptr;
    if (options == nullptr || error == nullptr) {
        return CVF_STATUS_INVALID_ARGUMENT;
    }
    if (const auto failure = validate_error_struct(error); failure.has_value()) {
        return status_code(failure.value());
    }
    if (const auto failure = validate_options_struct(options); failure.has_value()) {
        store_error(error, failure.value());
        return status_code(failure.value());
    }

    const std::string config_root(options->config_root_utf8, options->config_root_utf8_bytes);
    const std::string output_root(options->output_root_utf8, options->output_root_utf8_bytes);

    /*
     * The single-live-context check and the whole initialization run under the
     * lifecycle mutex, so two concurrent callers can never open two cameras or
     * publish two contexts.
     */
    std::lock_guard<std::mutex> guard(capi::context_mutex());
    if (capi::live_context_slot() != nullptr) {
        const core::Failure failure = core::make_failure(core::Status::context_limit,
                                                         core::ErrorCode::context_limit_reached,
                                                         "a live context already exists in this process");
        store_error(error, failure);
        return status_code(failure);
    }

    runtime::RuntimeOptions runtime_options;
    runtime_options.config_root = config_root;
    runtime_options.output_root = output_root;
    runtime_options.enable_file_logging = (options->flags & CVF_INIT_FLAG_FILE_LOGGING) != 0u;
    runtime_options.enable_callback_logging = (options->flags & CVF_INIT_FLAG_CALLBACK_LOGGING) != 0u;
    runtime_options.log_callback = diagnostics::CallbackBinding{options->log_callback, options->user_data};

    core::Result<std::unique_ptr<runtime::Context>> created = runtime::Context::create(std::move(runtime_options));
    if (!created.has_value()) {
        store_error(error, created.failure());
        return status_code(created.failure());
    }

    std::unique_ptr<cvf_context> context = std::make_unique<cvf_context>();
    context->state.config_root = config_root;
    context->state.output_root = output_root;
    context->state.flags = options->flags;
    context->state.log_callback = options->log_callback;
    context->state.user_data = options->user_data;
    context->state.runtime = std::move(created).value();

    capi::live_context_slot() = &context->state;
    *out_context = context.release();
    return CVF_STATUS_OK;
}

cvf_status_t CVF_CALL reload_recipes_impl(cvf_context* context, cvf_error_info_v1* error)
{
    if (error == nullptr) {
        return context == nullptr ? CVF_STATUS_INVALID_CONTEXT : CVF_STATUS_INVALID_ARGUMENT;
    }
    if (const auto failure = validate_error_struct(error); failure.has_value()) {
        return status_code(failure.value());
    }
    if (context == nullptr || !capi::is_live_context(context)) {
        const core::Failure failure = core::make_failure(core::Status::invalid_context,
                                                         core::ErrorCode::context_invalid,
                                                         "the context is NULL or no longer valid");
        store_error(error, failure);
        return status_code(failure);
    }
    core::Result<void> reloaded = context->state.runtime->reload_recipes();
    if (!reloaded.has_value()) {
        store_error(error, reloaded.failure());
        return status_code(reloaded.failure());
    }
    clear_error(error);
    return CVF_STATUS_OK;
}

cvf_status_t CVF_CALL inspect_impl(cvf_context* context, const cvf_inspection_request_v1* request,
                                   cvf_inspection_result_v1* result)
{
    /*
     * The context is checked before anything else, and the status is mirrored
     * into a writable result before returning, so a technical error can never
     * be observed as PASS/FAIL.
     */
    if (context == nullptr || !capi::is_live_context(context)) {
        const core::Failure failure = core::make_failure(core::Status::invalid_context,
                                                         core::ErrorCode::context_invalid,
                                                         "the context is NULL or no longer valid");
        publish_result_failure(result, failure);
        return status_code(failure);
    }
    if (request == nullptr || result == nullptr) {
        const core::Failure failure =
            core::invalid_argument(core::ErrorCode::invalid_pointer, "request and result are required");
        publish_result_failure(result, failure);
        return status_code(failure);
    }
    if (result->struct_size != kExpectedResultSize) {
        const core::Failure failure =
            core::invalid_argument(core::ErrorCode::struct_size_mismatch,
                                   "cvf_inspection_result_v1.struct_size does not match the v1 size");
        publish_result_failure(result, failure);
        return status_code(failure);
    }
    if (!reserved_is_zero(result)) {
        const core::Failure failure = core::invalid_argument(core::ErrorCode::reserved_not_zero,
                                                             "cvf_inspection_result_v1.reserved must be zero");
        publish_result_failure(result, failure);
        return status_code(failure);
    }
    if (const auto problem = result_capacity_problem(result); problem.has_value()) {
        publish_result_failure(result, problem->first, problem->second);
        return status_code(problem->first);
    }
    if (const auto failure = validate_request_struct(request); failure.has_value()) {
        publish_result_failure(result, failure.value());
        return status_code(failure.value());
    }

    core::Result<runtime::InspectionOutcome> outcome =
        context->state.runtime->inspect(make_runtime_request(request));
    if (!outcome.has_value()) {
        publish_result_failure(result, outcome.failure());
        return status_code(outcome.failure());
    }
    const runtime::InspectionOutcome& value = outcome.value();

    /*
     * The runtime produced exactly one canonical serialization and enforced the
     * 65535-byte payload bound. The C API copies that string verbatim and never
     * re-serializes the object; the caller capacity was validated to the v1
     * required capacity before any side effect.
     */
    const std::string& serialized_json = value.output_text;
    if (!serialized_json.empty() && serialized_json.size() + 1u > result->output_json_capacity) {
        const core::Failure failure =
            core::make_failure(core::Status::buffer_too_small, core::ErrorCode::runtime_result_too_large,
                               "the serialized inspection result exceeds the caller output_json capacity");
        publish_result_failure(result, failure);
        result->output_json_bytes_required = static_cast<std::uint32_t>(serialized_json.size() + 1u);
        return CVF_STATUS_BUFFER_TOO_SMALL;
    }

    result->status = core::to_public_status(value.status);
    result->verdict = core::to_public_verdict(value.verdict);
    result->error_code = core::to_public_error_code(value.error_code);
    result->warning_flags = value.warning_flags;
    result->elapsed_ms = value.elapsed_ms;
    write_result_text(result->output_json, result->output_json_capacity, serialized_json,
                      result->output_json_bytes_written, result->output_json_bytes_required);
    write_result_text(result->image_path, result->image_path_capacity, value.image_path,
                      result->image_path_bytes_written, result->image_path_bytes_required);
    if (value.status == core::Status::ok) {
        clear_result_text(result->error_message, result->error_message_capacity, result->error_message_bytes_written,
                          result->error_message_bytes_required);
    } else {
        write_result_text(result->error_message, result->error_message_capacity, value.error_message,
                          result->error_message_bytes_written, result->error_message_bytes_required);
    }
    return result->status;
}

cvf_status_t CVF_CALL shutdown_impl(cvf_context* context)
{
    if (context == nullptr) {
        return CVF_STATUS_INVALID_CONTEXT;
    }
    /*
     * The lifecycle mutex keeps the check-and-remove atomic and keeps the
     * orderly runtime teardown from racing a concurrent initialize.
     */
    std::lock_guard<std::mutex> guard(capi::context_mutex());
    if (capi::live_context_slot() != &context->state || context->state.magic != capi::kContextMagic) {
        return CVF_STATUS_INVALID_CONTEXT;
    }
    capi::live_context_slot() = nullptr;
    context->state.magic = 0u;
    std::unique_ptr<cvf_context> owned(context);
    return CVF_STATUS_OK;
}

}  // namespace

/* ---------------------------------------------------------------------- */
/* Exported symbols. Only these five names leave the shared library.       */
/* ---------------------------------------------------------------------- */

extern "C" {

CVF_API std::uint32_t CVF_CALL cvf_get_abi_version(void)
{
    try {
        return get_abi_version_impl();
    } catch (...) {
        return 0u;
    }
}

CVF_API cvf_status_t CVF_CALL cvf_initialize(const cvf_init_options_v1* options, cvf_context** out_context,
                                             cvf_error_info_v1* error)
{
    try {
        return initialize_impl(options, out_context, error);
    } catch (...) {
        return CVF_STATUS_INTERNAL_ERROR;
    }
}

CVF_API cvf_status_t CVF_CALL cvf_reload_recipes(cvf_context* context, cvf_error_info_v1* error)
{
    try {
        return reload_recipes_impl(context, error);
    } catch (...) {
        return CVF_STATUS_INTERNAL_ERROR;
    }
}

CVF_API cvf_status_t CVF_CALL cvf_inspect(cvf_context* context, const cvf_inspection_request_v1* request,
                                          cvf_inspection_result_v1* result)
{
    try {
        return inspect_impl(context, request, result);
    } catch (...) {
        const core::Failure failure = core::make_failure(core::Status::internal_error,
                                                         core::ErrorCode::internal_exception,
                                                         "an unexpected internal exception was contained");
        publish_result_failure(result, failure);
        return CVF_STATUS_INTERNAL_ERROR;
    }
}

CVF_API cvf_status_t CVF_CALL cvf_shutdown(cvf_context* context)
{
    try {
        return shutdown_impl(context);
    } catch (...) {
        return CVF_STATUS_INTERNAL_ERROR;
    }
}

}  // extern "C"

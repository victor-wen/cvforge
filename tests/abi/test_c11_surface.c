/*
 * CVF-001 independent black-box test - brief B1 / B3 / B4.
 *
 * C11 compile-time public-surface conformance for <cvforwin/cvf_api.h>:
 * names every documented function, struct, field, constant and callback type.
 *
 * Authored by test-engineer from .ai/test-briefs/CVF-001.yaml and the approved
 * .ai/project-contract.yaml only; no production implementation was consulted.
 *
 * RED / verify command (header must exist for this to compile):
 *   gcc -std=c11 -Wall -Wextra -Werror -I include -fsyntax-only tests/abi/test_c11_surface.c
 *
 * When linked against the cvforwin runtime this file is also a self-checking
 * executable: it exits 0 when every compile-time property below holds.
 */
#include <cvforwin/cvf_api.h>

#include <stdint.h>
#include <string.h>

/* ---- B3: exact numeric constants ---- */

_Static_assert(CVF_ABI_VERSION_V1 == 1, "CVF_ABI_VERSION_V1 must be 1");
_Static_assert(CVF_RECIPE_ID_MAX_UTF8_BYTES == 128u, "CVF_RECIPE_ID_MAX_UTF8_BYTES must be 128");
_Static_assert(CVF_REQUEST_ID_MAX_UTF8_BYTES == 128u, "CVF_REQUEST_ID_MAX_UTF8_BYTES must be 128");
_Static_assert(CVF_INPUT_JSON_MAX_UTF8_BYTES == 65536u, "CVF_INPUT_JSON_MAX_UTF8_BYTES must be 65536");
_Static_assert(CVF_RESULT_JSON_REQUIRED_CAPACITY == 65536u, "CVF_RESULT_JSON_REQUIRED_CAPACITY must be 65536");
_Static_assert(CVF_ERROR_MESSAGE_REQUIRED_CAPACITY == 1024u, "CVF_ERROR_MESSAGE_REQUIRED_CAPACITY must be 1024");
_Static_assert(CVF_IMAGE_PATH_REQUIRED_CAPACITY == 4096u, "CVF_IMAGE_PATH_REQUIRED_CAPACITY must be 4096");

_Static_assert(CVF_STATUS_OK == 0u, "CVF_STATUS_OK must be 0");
_Static_assert(CVF_STATUS_INVALID_ARGUMENT == 1u, "CVF_STATUS_INVALID_ARGUMENT must be 1");
_Static_assert(CVF_STATUS_ABI_MISMATCH == 2u, "CVF_STATUS_ABI_MISMATCH must be 2");
_Static_assert(CVF_STATUS_CONTEXT_LIMIT == 3u, "CVF_STATUS_CONTEXT_LIMIT must be 3");
_Static_assert(CVF_STATUS_INVALID_CONTEXT == 4u, "CVF_STATUS_INVALID_CONTEXT must be 4");
_Static_assert(CVF_STATUS_CONFIG_ERROR == 5u, "CVF_STATUS_CONFIG_ERROR must be 5");
_Static_assert(CVF_STATUS_RECIPE_NOT_FOUND == 6u, "CVF_STATUS_RECIPE_NOT_FOUND must be 6");
_Static_assert(CVF_STATUS_CAMERA_NOT_FOUND == 7u, "CVF_STATUS_CAMERA_NOT_FOUND must be 7");
_Static_assert(CVF_STATUS_CAMERA_IO == 8u, "CVF_STATUS_CAMERA_IO must be 8");
_Static_assert(CVF_STATUS_TIMEOUT == 9u, "CVF_STATUS_TIMEOUT must be 9");
_Static_assert(CVF_STATUS_BUFFER_TOO_SMALL == 10u, "CVF_STATUS_BUFFER_TOO_SMALL must be 10");
_Static_assert(CVF_STATUS_ALGORITHM_ERROR == 11u, "CVF_STATUS_ALGORITHM_ERROR must be 11");
_Static_assert(CVF_STATUS_REQUIRED_ARTIFACT_ERROR == 12u, "CVF_STATUS_REQUIRED_ARTIFACT_ERROR must be 12");
_Static_assert(CVF_STATUS_INTERNAL_ERROR == 13u, "CVF_STATUS_INTERNAL_ERROR must be 13");

_Static_assert(CVF_VERDICT_NOT_EVALUATED == 0u, "CVF_VERDICT_NOT_EVALUATED must be 0");
_Static_assert(CVF_VERDICT_PASS == 1u, "CVF_VERDICT_PASS must be 1");
_Static_assert(CVF_VERDICT_FAIL == 2u, "CVF_VERDICT_FAIL must be 2");

_Static_assert(CVF_INIT_FLAG_FILE_LOGGING == 1u, "CVF_INIT_FLAG_FILE_LOGGING must be 1u << 0");
_Static_assert(CVF_INIT_FLAG_CALLBACK_LOGGING == 2u, "CVF_INIT_FLAG_CALLBACK_LOGGING must be 1u << 1");

/* ---- B4: exact function signatures ---- */

typedef uint32_t (*cvf_get_abi_version_fn)(void);
typedef cvf_status_t (*cvf_initialize_fn)(const cvf_init_options_v1*, cvf_context**, cvf_error_info_v1*);
typedef cvf_status_t (*cvf_reload_recipes_fn)(cvf_context*, cvf_error_info_v1*);
typedef cvf_status_t (*cvf_inspect_fn)(cvf_context*, const cvf_inspection_request_v1*, cvf_inspection_result_v1*);
typedef cvf_status_t (*cvf_shutdown_fn)(cvf_context*);

/* Callback type: void (CVF_CALL*)(uint32_t level, const char* message_utf8, void* user_data). */
static void CVF_CALL cvf_c11_log_callback(uint32_t level, const char* message_utf8, void* user_data)
{
    (void)level;
    (void)message_utf8;
    (void)user_data;
}

static int signatures_compile(void)
{
    cvf_get_abi_version_fn p_get_abi_version = &cvf_get_abi_version;
    cvf_initialize_fn p_initialize = &cvf_initialize;
    cvf_reload_recipes_fn p_reload_recipes = &cvf_reload_recipes;
    cvf_inspect_fn p_inspect = &cvf_inspect;
    cvf_shutdown_fn p_shutdown = &cvf_shutdown;

    return p_get_abi_version != NULL
        && p_initialize != NULL
        && p_reload_recipes != NULL
        && p_inspect != NULL
        && p_shutdown != NULL;
}

static int callback_type_compiles(void)
{
    cvf_log_callback callback = &cvf_c11_log_callback;
    cvf_init_options_v1 options;

    memset(&options, 0, sizeof options);
    options.log_callback = callback;
    return options.log_callback != NULL;
}

/* ---- B1: every documented struct and field ---- */

static void fields_name_and_assign(void)
{
    cvf_init_options_v1 init_options;
    cvf_error_info_v1 error_info;
    cvf_inspection_request_v1 request;
    cvf_inspection_result_v1 result;

    memset(&init_options, 0, sizeof init_options);
    init_options.struct_size = (uint32_t)sizeof init_options;
    init_options.abi_version = CVF_ABI_VERSION_V1;
    init_options.config_root_utf8 = NULL;
    init_options.config_root_utf8_bytes = 0u;
    init_options.output_root_utf8 = NULL;
    init_options.output_root_utf8_bytes = 0u;
    init_options.flags = CVF_INIT_FLAG_FILE_LOGGING;
    init_options.log_callback = &cvf_c11_log_callback;
    init_options.user_data = NULL;
    init_options.reserved[0] = 0u;
    init_options.reserved[7] = 0u;

    memset(&error_info, 0, sizeof error_info);
    error_info.struct_size = (uint32_t)sizeof error_info;
    error_info.error_code = 0u;
    error_info.message_utf8 = NULL;
    error_info.message_capacity = 0u;
    error_info.message_bytes_written = 0u;
    error_info.message_bytes_required = 0u;
    error_info.reserved[0] = 0u;
    error_info.reserved[3] = 0u;

    memset(&request, 0, sizeof request);
    request.struct_size = (uint32_t)sizeof request;
    request.abi_version = CVF_ABI_VERSION_V1;
    request.recipe_id_utf8 = NULL;
    request.recipe_id_utf8_bytes = 0u;
    request.request_id_utf8 = NULL;
    request.request_id_utf8_bytes = 0u;
    request.timeout_ms = 0u;
    request.input_json_utf8 = NULL;
    request.input_json_utf8_bytes = 0u;
    request.reserved[0] = 0u;
    request.reserved[7] = 0u;

    memset(&result, 0, sizeof result);
    result.struct_size = (uint32_t)sizeof result;
    result.status = CVF_STATUS_OK;
    result.verdict = CVF_VERDICT_NOT_EVALUATED;
    result.error_code = 0u;
    result.warning_flags = 0u;
    result.elapsed_ms = 0u;
    result.output_json = NULL;
    result.output_json_capacity = 0u;
    result.output_json_bytes_written = 0u;
    result.output_json_bytes_required = 0u;
    result.image_path = NULL;
    result.image_path_capacity = 0u;
    result.image_path_bytes_written = 0u;
    result.image_path_bytes_required = 0u;
    result.error_message = NULL;
    result.error_message_capacity = 0u;
    result.error_message_bytes_written = 0u;
    result.error_message_bytes_required = 0u;
    result.reserved[0] = 0u;
    result.reserved[7] = 0u;

    /* Consume the values so strict -Wall -Wextra -Werror does not flag them. */
    (void)init_options;
    (void)error_info;
    (void)request;
    (void)result;
}

int main(void)
{
    fields_name_and_assign();
    if (!callback_type_compiles()) {
        return 1;
    }
    if (!signatures_compile()) {
        return 1;
    }
    return 0;
}

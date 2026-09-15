/*
 * CVF-001 independent black-box test - brief B2 / B5 / B6 / B7 / B8.
 *
 * Stateless validation boundary of the public C ABI v1:
 *   - ABI version without initialization,
 *   - precondition rejection (null pointers, struct_size, abi_version,
 *     reserved fields, unknown flags, CALLBACK_LOGGING without callback),
 *   - cvf_error_info_v1 buffer reporting rules,
 *   - null-context handling and no-fake-OK behavior.
 *
 * Authored by test-engineer from .ai/test-briefs/CVF-001.yaml and the approved
 * .ai/project-contract.yaml only; no production implementation was consulted.
 *
 * RED / verify commands (header and library must exist):
 *   gcc -std=c11 -Wall -Wextra -Werror -I include -fsyntax-only tests/abi/test_abi_runtime.c
 *   link this file against the cvforwin runtime, then run the produced executable.
 *
 * Exercises are deterministic and hardware-free; they require no camera and no
 * real configuration tree. A crash aborts the run, which is itself a failure.
 */
#include <cvforwin/cvf_api.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char kMissingConfigRoot[] = "/nonexistent/cvforwin-cvf001/config-root";
static const char kMissingOutputRoot[] = "/nonexistent/cvforwin-cvf001/output-root";
static const char kRecipeId[] = "example";
static const char kRequestId[] = "cvf001-request-1";

static char g_output_json[CVF_RESULT_JSON_REQUIRED_CAPACITY];
static char g_image_path[CVF_IMAGE_PATH_REQUIRED_CAPACITY];
static char g_result_error_message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];

static int g_checks = 0;
static int g_failures = 0;

static const char* status_name(cvf_status_t status)
{
    switch (status) {
    case CVF_STATUS_OK: return "CVF_STATUS_OK";
    case CVF_STATUS_INVALID_ARGUMENT: return "CVF_STATUS_INVALID_ARGUMENT";
    case CVF_STATUS_ABI_MISMATCH: return "CVF_STATUS_ABI_MISMATCH";
    case CVF_STATUS_CONTEXT_LIMIT: return "CVF_STATUS_CONTEXT_LIMIT";
    case CVF_STATUS_INVALID_CONTEXT: return "CVF_STATUS_INVALID_CONTEXT";
    case CVF_STATUS_CONFIG_ERROR: return "CVF_STATUS_CONFIG_ERROR";
    case CVF_STATUS_RECIPE_NOT_FOUND: return "CVF_STATUS_RECIPE_NOT_FOUND";
    case CVF_STATUS_CAMERA_NOT_FOUND: return "CVF_STATUS_CAMERA_NOT_FOUND";
    case CVF_STATUS_CAMERA_IO: return "CVF_STATUS_CAMERA_IO";
    case CVF_STATUS_TIMEOUT: return "CVF_STATUS_TIMEOUT";
    case CVF_STATUS_BUFFER_TOO_SMALL: return "CVF_STATUS_BUFFER_TOO_SMALL";
    case CVF_STATUS_ALGORITHM_ERROR: return "CVF_STATUS_ALGORITHM_ERROR";
    case CVF_STATUS_REQUIRED_ARTIFACT_ERROR: return "CVF_STATUS_REQUIRED_ARTIFACT_ERROR";
    case CVF_STATUS_INTERNAL_ERROR: return "CVF_STATUS_INTERNAL_ERROR";
    default: return "CVF_STATUS_<unknown>";
    }
}

static void check_status(const char* test_name, cvf_status_t observed, cvf_status_t expected)
{
    ++g_checks;
    if (observed != expected) {
        ++g_failures;
        printf("FAIL [%s] expected status %u (%s), observed %u (%s)\n",
               test_name, (unsigned)expected, status_name(expected),
               (unsigned)observed, status_name(observed));
    } else {
        printf("ok   [%s] status=%u (%s)\n", test_name, (unsigned)observed, status_name(observed));
    }
}

static void check_not_ok(const char* test_name, cvf_status_t observed)
{
    ++g_checks;
    if (observed == CVF_STATUS_OK) {
        ++g_failures;
        printf("FAIL [%s] expected a documented non-OK status, observed CVF_STATUS_OK\n", test_name);
    } else {
        printf("ok   [%s] non-OK status=%u (%s)\n", test_name, (unsigned)observed, status_name(observed));
    }
}

static void check_condition(const char* test_name, int condition, const char* expectation)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        printf("FAIL [%s] expected: %s\n", test_name, expectation);
    }
}

static void check_u32(const char* test_name, uint32_t observed, uint32_t expected, const char* what)
{
    ++g_checks;
    if (observed != expected) {
        ++g_failures;
        printf("FAIL [%s] expected %s = %u, observed %u\n",
               test_name, what, (unsigned)expected, (unsigned)observed);
    } else {
        printf("ok   [%s] %s = %u\n", test_name, what, (unsigned)observed);
    }
}

static cvf_init_options_v1 make_options(void)
{
    cvf_init_options_v1 options;

    memset(&options, 0, sizeof options);
    options.struct_size = (uint32_t)sizeof options;
    options.abi_version = CVF_ABI_VERSION_V1;
    options.config_root_utf8 = kMissingConfigRoot;
    options.config_root_utf8_bytes = (uint32_t)strlen(kMissingConfigRoot);
    options.output_root_utf8 = kMissingOutputRoot;
    options.output_root_utf8_bytes = (uint32_t)strlen(kMissingOutputRoot);
    return options;
}

static cvf_error_info_v1 make_error(char* buffer, uint32_t capacity)
{
    cvf_error_info_v1 error;

    memset(&error, 0, sizeof error);
    error.struct_size = (uint32_t)sizeof error;
    error.message_utf8 = buffer;
    error.message_capacity = capacity;
    return error;
}

static cvf_inspection_request_v1 make_request(void)
{
    cvf_inspection_request_v1 request;

    memset(&request, 0, sizeof request);
    request.struct_size = (uint32_t)sizeof request;
    request.abi_version = CVF_ABI_VERSION_V1;
    request.recipe_id_utf8 = kRecipeId;
    request.recipe_id_utf8_bytes = (uint32_t)strlen(kRecipeId);
    request.request_id_utf8 = kRequestId;
    request.request_id_utf8_bytes = (uint32_t)strlen(kRequestId);
    request.timeout_ms = 0u;
    request.input_json_utf8 = NULL;
    request.input_json_utf8_bytes = 0u;
    return request;
}

/* A result whose buffers have the documented v1 required capacities. */
static cvf_inspection_result_v1 make_result(void)
{
    cvf_inspection_result_v1 result;

    memset(&result, 0, sizeof result);
    result.struct_size = (uint32_t)sizeof result;
    result.output_json = g_output_json;
    result.output_json_capacity = (uint32_t)sizeof g_output_json;
    result.image_path = g_image_path;
    result.image_path_capacity = (uint32_t)sizeof g_image_path;
    result.error_message = g_result_error_message;
    result.error_message_capacity = (uint32_t)sizeof g_result_error_message;
    return result;
}

static void expect_initialize_rejected(const char* test_name, cvf_init_options_v1 options, cvf_status_t expected)
{
    char buffer[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
    cvf_error_info_v1 error = make_error(buffer, (uint32_t)sizeof buffer);
    cvf_context* context = NULL;
    cvf_status_t status = cvf_initialize(&options, &context, &error);

    check_status(test_name, status, expected);
    check_condition(test_name, context == NULL,
                    "out_context remains NULL when cvf_initialize rejects the call");
}

static void expect_initialize_rejected_error_struct(const char* test_name, cvf_init_options_v1 options,
                                                    uint32_t error_struct_size, cvf_status_t expected)
{
    char buffer[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
    cvf_error_info_v1 error = make_error(buffer, (uint32_t)sizeof buffer);
    cvf_context* context = NULL;
    cvf_status_t status;

    error.struct_size = error_struct_size;
    status = cvf_initialize(&options, &context, &error);
    check_status(test_name, status, expected);
    check_condition(test_name, context == NULL,
                    "out_context remains NULL when cvf_initialize rejects the call");
}

int main(void)
{
    uint32_t required_full = 0u;
    cvf_status_t status_full = CVF_STATUS_OK;

    /* ---- B2: ABI version needs neither context nor initialization ---- */
    {
        const char* name = "abi_version_is_1_without_initialization";
        uint32_t first = cvf_get_abi_version();
        uint32_t second = cvf_get_abi_version();
        check_u32(name, first, CVF_ABI_VERSION_V1, "first cvf_get_abi_version()");
        check_u32(name, second, CVF_ABI_VERSION_V1, "second cvf_get_abi_version()");
    }

    /* ---- B5 / negative: cvf_initialize precondition rejection ---- */
    {
        const char* name = "initialize_null_options_rejected";
        char buffer[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
        cvf_error_info_v1 error = make_error(buffer, (uint32_t)sizeof buffer);
        cvf_context* context = NULL;
        cvf_status_t status = cvf_initialize(NULL, &context, &error);

        check_status(name, status, CVF_STATUS_INVALID_ARGUMENT);
        check_condition(name, context == NULL, "out_context remains NULL when cvf_initialize rejects the call");
    }
    {
        const char* name = "initialize_null_out_context_rejected";
        char buffer[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
        cvf_error_info_v1 error = make_error(buffer, (uint32_t)sizeof buffer);
        cvf_init_options_v1 options = make_options();
        cvf_status_t status = cvf_initialize(&options, NULL, &error);

        check_status(name, status, CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        const char* name = "initialize_null_error_rejected";
        cvf_init_options_v1 options = make_options();
        cvf_context* context = NULL;
        cvf_status_t status = cvf_initialize(&options, &context, NULL);

        check_status(name, status, CVF_STATUS_INVALID_ARGUMENT);
        check_condition(name, context == NULL, "out_context remains NULL when the error struct pointer is NULL");
    }
    {
        cvf_init_options_v1 options = make_options();
        options.struct_size = 0u;
        expect_initialize_rejected("initialize_struct_size_zero_rejected", options, CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_init_options_v1 options = make_options();
        options.struct_size = (uint32_t)sizeof(options) - 1u;
        expect_initialize_rejected("initialize_struct_size_minus_one_rejected", options, CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_init_options_v1 options = make_options();
        options.abi_version = 0u;
        expect_initialize_rejected("initialize_abi_version_zero_rejected", options, CVF_STATUS_ABI_MISMATCH);
    }
    {
        cvf_init_options_v1 options = make_options();
        options.abi_version = 2u;
        expect_initialize_rejected("initialize_abi_version_two_rejected", options, CVF_STATUS_ABI_MISMATCH);
    }
    {
        const char* name = "initialize_reserved_nonzero_not_ok";
        char buffer[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
        cvf_error_info_v1 error = make_error(buffer, (uint32_t)sizeof buffer);
        cvf_init_options_v1 options = make_options();
        cvf_context* context = NULL;
        cvf_status_t status;

        memset(options.reserved, 0xFF, sizeof options.reserved);
        status = cvf_initialize(&options, &context, &error);
        check_not_ok(name, status);
        check_condition(name, context == NULL, "out_context remains NULL when reserved fields are nonzero");
    }
    {
        cvf_init_options_v1 options = make_options();
        options.flags = 0x80000000u; /* unknown flag bit */
        expect_initialize_rejected("initialize_unknown_flag_bit_rejected", options, CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_init_options_v1 options = make_options();
        options.flags = CVF_INIT_FLAG_CALLBACK_LOGGING;
        options.log_callback = NULL;
        options.user_data = NULL;
        expect_initialize_rejected("initialize_callback_flag_without_callback_rejected", options,
                                   CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_init_options_v1 options = make_options();
        expect_initialize_rejected_error_struct("initialize_error_struct_size_zero_rejected", options, 0u,
                                                CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_init_options_v1 options = make_options();
        expect_initialize_rejected_error_struct("initialize_error_struct_size_minus_one_rejected", options,
                                                (uint32_t)sizeof(cvf_error_info_v1) - 1u,
                                                CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        const char* name = "initialize_error_null_buffer_with_capacity_rejected";
        cvf_init_options_v1 options = make_options();
        cvf_error_info_v1 error = make_error(NULL, CVF_ERROR_MESSAGE_REQUIRED_CAPACITY);
        cvf_context* context = NULL;
        cvf_status_t status = cvf_initialize(&options, &context, &error);

        check_status(name, status, CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        const char* name = "initialize_error_reserved_nonzero_not_ok";
        char buffer[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
        cvf_error_info_v1 error = make_error(buffer, (uint32_t)sizeof buffer);
        cvf_init_options_v1 options = make_options();
        cvf_context* context = NULL;
        cvf_status_t status;

        memset(error.reserved, 0xFF, sizeof error.reserved);
        status = cvf_initialize(&options, &context, &error);
        check_not_ok(name, status);
        check_condition(name, context == NULL,
                        "out_context remains NULL when error reserved fields are nonzero");
    }

    /* ---- B8 + B6: failed initialize reports a message with documented byte rules ---- */
    {
        const char* name = "initialize_missing_config_root_reports_error";
        char buffer[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
        cvf_error_info_v1 error = make_error(buffer, (uint32_t)sizeof buffer);
        cvf_init_options_v1 options = make_options();
        cvf_context* context = NULL;
        cvf_status_t status = cvf_initialize(&options, &context, &error);
        int written_in_range = error.message_bytes_written < (uint32_t)sizeof buffer;

        check_not_ok(name, status);
        check_condition(name, context == NULL, "out_context is NULL after a failed initialize");
        check_condition(name, error.error_code != 0u, "error_code is nonzero when cvf_initialize fails");
        check_condition(name, error.message_bytes_written <= error.message_capacity,
                        "message_bytes_written never exceeds message_capacity");
        check_condition(name, written_in_range,
                        "message_bytes_written stays inside the caller-provided buffer");
        if (written_in_range) {
            check_condition(name, buffer[error.message_bytes_written] == '\0',
                            "the written error message is NUL-terminated at message_bytes_written");
        }
        check_condition(name, error.message_bytes_required >= error.message_bytes_written + 1u,
                        "message_bytes_required includes the trailing NUL");
        check_condition(name, error.message_bytes_required == error.message_bytes_written + 1u,
                        "a message that fits CVF_ERROR_MESSAGE_REQUIRED_CAPACITY reports required == written + 1");
        status_full = status;
        required_full = error.message_bytes_required;
    }
    {
        const char* name = "initialize_error_message_capacity_zero";
        cvf_error_info_v1 error = make_error(NULL, 0u);
        cvf_init_options_v1 options = make_options();
        cvf_context* context = NULL;
        cvf_status_t status = cvf_initialize(&options, &context, &error);

        check_status(name, status, status_full);
        check_condition(name, context == NULL, "out_context is NULL after a failed initialize");
        check_u32(name, error.message_bytes_written, 0u, "message_bytes_written with capacity 0");
        check_u32(name, error.message_bytes_required, required_full,
                  "message_bytes_required with capacity 0 equals the full-capacity run");
    }
    {
        const char* name = "initialize_error_message_capacity_one";
        char tiny[1];
        cvf_error_info_v1 error = make_error(tiny, 1u);
        cvf_init_options_v1 options = make_options();
        cvf_context* context = NULL;
        cvf_status_t status;

        tiny[0] = 'X';
        status = cvf_initialize(&options, &context, &error);
        check_status(name, status, status_full);
        check_condition(name, tiny[0] == '\0', "a capacity-1 buffer holds only the terminating NUL");
        check_u32(name, error.message_bytes_written, 0u, "message_bytes_written with capacity 1");
        check_u32(name, error.message_bytes_required, required_full,
                  "message_bytes_required with capacity 1 equals the full-capacity run");
    }

    /* ---- B7: null-context handling and no-fake-OK ---- */
    {
        const char* name = "reload_recipes_null_context_invalid_context";
        char buffer[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
        cvf_error_info_v1 error = make_error(buffer, (uint32_t)sizeof buffer);
        cvf_status_t status = cvf_reload_recipes(NULL, &error);

        check_status(name, status, CVF_STATUS_INVALID_CONTEXT);
    }
    {
        const char* name = "reload_recipes_null_context_null_error_not_ok";
        cvf_status_t status = cvf_reload_recipes(NULL, NULL);

        check_not_ok(name, status);
    }
    {
        const char* name = "shutdown_null_context_invalid_context";
        cvf_status_t status = cvf_shutdown(NULL);

        check_status(name, status, CVF_STATUS_INVALID_CONTEXT);
    }
    {
        const char* name = "inspect_null_context_invalid_context";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status = cvf_inspect(NULL, &request, &result);

        check_status(name, status, CVF_STATUS_INVALID_CONTEXT);
    }
    {
        const char* name = "inspect_null_context_verdict_not_evaluated";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        result.status = CVF_STATUS_OK;
        result.verdict = CVF_VERDICT_PASS;
        status = cvf_inspect(NULL, &request, &result);
        check_condition(name, status != CVF_STATUS_OK,
                        "cvf_inspect with a null context does not return CVF_STATUS_OK");
        check_u32(name, (uint32_t)result.verdict, (uint32_t)CVF_VERDICT_NOT_EVALUATED,
                  "result.verdict after a non-OK inspect (technical errors are never PASS/FAIL)");
    }
    {
        const char* name = "inspect_null_context_null_request_null_result_not_ok";
        cvf_status_t status = cvf_inspect(NULL, NULL, NULL);

        check_not_ok(name, status);
    }
    /*
     * Boundary group below: a malformed request/result plus a null context has an
     * underdetermined exact status in the brief (struct validation or context
     * validation may run first), so only the derivable properties are asserted:
     * no crash and no fake CVF_STATUS_OK.
     */
    {
        const char* name = "inspect_null_context_struct_size_zero_not_ok";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        request.struct_size = 0u;
        status = cvf_inspect(NULL, &request, &result);
        check_not_ok(name, status);
    }
    {
        const char* name = "inspect_null_context_result_struct_size_zero_not_ok";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        result.struct_size = 0u;
        status = cvf_inspect(NULL, &request, &result);
        check_not_ok(name, status);
    }
    {
        const char* name = "inspect_null_context_abi_version_two_not_ok";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        request.abi_version = 2u;
        status = cvf_inspect(NULL, &request, &result);
        check_not_ok(name, status);
    }
    {
        const char* name = "inspect_null_context_request_abi_version_zero_not_ok";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        request.abi_version = 0u;
        status = cvf_inspect(NULL, &request, &result);
        check_not_ok(name, status);
    }
    {
        const char* name = "inspect_null_context_request_struct_size_minus_one_not_ok";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        request.struct_size = (uint32_t)sizeof(request) - 1u;
        status = cvf_inspect(NULL, &request, &result);
        check_not_ok(name, status);
    }
    {
        const char* name = "inspect_null_context_result_struct_size_minus_one_not_ok";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        result.struct_size = (uint32_t)sizeof(result) - 1u;
        status = cvf_inspect(NULL, &request, &result);
        check_not_ok(name, status);
    }
    {
        const char* name = "inspect_null_context_request_reserved_nonzero_not_ok";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        memset(request.reserved, 0xFF, sizeof request.reserved);
        status = cvf_inspect(NULL, &request, &result);
        check_not_ok(name, status);
    }
    {
        const char* name = "inspect_null_context_result_reserved_nonzero_not_ok";
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        cvf_status_t status;

        memset(result.reserved, 0xFF, sizeof result.reserved);
        status = cvf_inspect(NULL, &request, &result);
        check_not_ok(name, status);
    }

    printf("test_abi_runtime: checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

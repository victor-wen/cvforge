/*
 * CVF-008 clean package-consumer kit - installed-package C11 consumer.
 *
 * This translation unit is the consumer half of the package-consumer check.
 * It includes only <cvforwin/cvf_api.h> from the installed package, links the
 * installed import library, and drives the public C ABI v1 lifecycle:
 *
 *   query ABI version -> initialize (synthetic, test-enabled config) ->
 *   one inspect -> reload recipes -> shutdown
 *
 * It never includes a project header other than cvf_api.h and never reads the
 * main build tree, the source tree, or an internal type.
 *
 * Usage (both roots are required absolute paths created by the caller):
 *   cvf_package_consumer <absolute config root> <absolute output root>
 *
 * Exit codes (deterministic):
 *   0  every observable expectation held
 *   1  the library or an expectation failed (details printed)
 *   2  usage error (missing or non-absolute root arguments)
 *
 * Expectations pinned from the approved contract and the CVF-008 brief:
 *   - cvf_get_abi_version() returns CVF_ABI_VERSION_V1 before initialization;
 *   - cvf_initialize with the synthetic config returns CVF_STATUS_OK;
 *   - one cvf_inspect call returns CVF_STATUS_OK with result.status OK,
 *     error_code 0, and a PASS verdict (never NOT_EVALUATED on success) for a
 *     recipe whose min_pass_ratio is 0.0; the JSON is a NUL-terminated object
 *     carrying the example measurements, and save_policy "never" leaves the
 *     image path empty;
 *   - cvf_reload_recipes returns CVF_STATUS_OK with error_code 0;
 *   - cvf_shutdown returns CVF_STATUS_OK.
 *
 * The synthetic backend is accepted only by a test-enabled package build
 * (CVFORWIN_BUILD_TEST_BACKENDS=ON); CI wiring is documented in
 * tests/package/package_checks.cmake.
 */
#include <cvforwin/cvf_api.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    CVF_CONSUMER_EXIT_OK = 0,
    CVF_CONSUMER_EXIT_CHECK_FAILED = 1,
    CVF_CONSUMER_EXIT_USAGE = 2
};

static const char kRecipeId[] = "example.pass";
static const char kRequestId[] = "package-consumer-1";

static char g_output_json[CVF_RESULT_JSON_REQUIRED_CAPACITY];
static char g_image_path[CVF_IMAGE_PATH_REQUIRED_CAPACITY];
static char g_result_message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
static char g_error_message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];

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

static void check_status(const char* name, cvf_status_t observed, cvf_status_t expected)
{
    ++g_checks;
    if (observed != expected) {
        ++g_failures;
        printf("FAIL [%s] expected status %u (%s), observed %u (%s)\n", name, (unsigned)expected,
               status_name(expected), (unsigned)observed, status_name(observed));
    }
}

static void check_u32(const char* name, uint32_t observed, uint32_t expected, const char* what)
{
    ++g_checks;
    if (observed != expected) {
        ++g_failures;
        printf("FAIL [%s] expected %s = %u, observed %u\n", name, what, (unsigned)expected,
               (unsigned)observed);
    }
}

static void check_condition(const char* name, int condition, const char* expectation)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        printf("FAIL [%s] expected: %s\n", name, expectation);
    }
}

static int is_absolute_path(const char* path)
{
#ifdef _WIN32
    if (path[0] == '\\' || path[0] == '/') {
        return 1;
    }
    return path[0] != '\0' && path[1] == ':';
#else
    return path[0] == '/';
#endif
}

static int json_contains_key(const char* text, const char* key)
{
    char needle[64];

    snprintf(needle, sizeof needle, "\"%s\"", key);
    return strstr(text, needle) != NULL;
}

static cvf_init_options_v1 make_options(const char* config_root, const char* output_root)
{
    cvf_init_options_v1 options;

    memset(&options, 0, sizeof options);
    options.struct_size = (uint32_t)sizeof options;
    options.abi_version = CVF_ABI_VERSION_V1;
    options.config_root_utf8 = config_root;
    options.config_root_utf8_bytes = (uint32_t)strlen(config_root);
    options.output_root_utf8 = output_root;
    options.output_root_utf8_bytes = (uint32_t)strlen(output_root);
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

static cvf_inspection_result_v1 make_result(void)
{
    cvf_inspection_result_v1 result;

    memset(&result, 0, sizeof result);
    result.struct_size = (uint32_t)sizeof result;
    result.output_json = g_output_json;
    result.output_json_capacity = (uint32_t)sizeof g_output_json;
    result.image_path = g_image_path;
    result.image_path_capacity = (uint32_t)sizeof g_image_path;
    result.error_message = g_result_message;
    result.error_message_capacity = (uint32_t)sizeof g_result_message;
    return result;
}

static int run_lifecycle(const char* config_root, const char* output_root)
{
    cvf_error_info_v1 error = make_error(g_error_message, (uint32_t)sizeof g_error_message);
    cvf_init_options_v1 options = make_options(config_root, output_root);
    cvf_context* context = NULL;
    cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

    /* P2: the ABI version is available before any initialization. */
    check_u32("abi_version_before_initialize", cvf_get_abi_version(), CVF_ABI_VERSION_V1,
              "cvf_get_abi_version()");

    /* P1/P3: initialize against the installed package's synthetic config. */
    status = cvf_initialize(&options, &context, &error);
    check_status("initialize_synthetic_config", status, CVF_STATUS_OK);
    if (status != CVF_STATUS_OK || context == NULL) {
        printf("initialize failed: error_code=%u message=\"%s\"\n", (unsigned)error.error_code,
               error.message_utf8 != NULL ? error.message_utf8 : "(no message)");
        return CVF_CONSUMER_EXIT_CHECK_FAILED;
    }

    /* P3: one inspect on the deterministic synthetic frame. */
    {
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();

        status = cvf_inspect(context, &request, &result);
        check_status("inspect_status", status, CVF_STATUS_OK);
        check_u32("inspect_result_status", (uint32_t)result.status, (uint32_t)CVF_STATUS_OK,
                  "result.status");
        check_u32("inspect_result_error_code", result.error_code, 0u, "result.error_code");
        check_u32("inspect_verdict_is_pass", (uint32_t)result.verdict, (uint32_t)CVF_VERDICT_PASS,
                  "result.verdict (never NOT_EVALUATED on success)");
        check_condition("inspect_json_bounded",
                        result.output_json_bytes_written > 0u &&
                            result.output_json_bytes_written < result.output_json_capacity,
                        "output_json_bytes_written is nonzero and below the capacity");
        if (result.output_json_bytes_written < result.output_json_capacity) {
            check_condition("inspect_json_nul_terminated",
                            result.output_json[result.output_json_bytes_written] == '\0',
                            "output_json is NUL-terminated at bytes_written");
        }
        check_u32("inspect_json_required", result.output_json_bytes_required,
                  result.output_json_bytes_written + 1u,
                  "output_json_bytes_required (includes the trailing NUL)");
        check_condition("inspect_json_is_object", result.output_json[0] == '{',
                        "output_json is a UTF-8 JSON object");
        check_condition("inspect_json_has_measurements",
                        json_contains_key(result.output_json, "white_pixels") &&
                            json_contains_key(result.output_json, "total_pixels") &&
                            json_contains_key(result.output_json, "pass_ratio"),
                        "output_json contains white_pixels, total_pixels, and pass_ratio");
        check_u32("inspect_no_saved_image", result.image_path_bytes_written, 0u,
                  "image_path_bytes_written for save_policy never");
        printf("inspect: verdict=%u output_json=%s\n", (unsigned)result.verdict, result.output_json);
    }

    /* P3: atomic recipe reload succeeds and reports no error. */
    {
        cvf_error_info_v1 reload_error =
            make_error(g_error_message, (uint32_t)sizeof g_error_message);
        cvf_status_t reload_status = cvf_reload_recipes(context, &reload_error);

        check_status("reload_recipes", reload_status, CVF_STATUS_OK);
        check_u32("reload_recipes_error_code", reload_error.error_code, 0u,
                  "error.error_code after a successful reload");
    }

    /* P1: orderly shutdown of the single context. */
    check_status("shutdown", cvf_shutdown(context), CVF_STATUS_OK);

    return g_failures == 0 ? CVF_CONSUMER_EXIT_OK : CVF_CONSUMER_EXIT_CHECK_FAILED;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s <absolute config root> <absolute output root>\n"
                "  both roots must be absolute paths to existing directories\n",
                argv[0] != NULL ? argv[0] : "cvf_package_consumer");
        return CVF_CONSUMER_EXIT_USAGE;
    }
    if (!is_absolute_path(argv[1]) || !is_absolute_path(argv[2])) {
        fprintf(stderr,
                "usage error: the config root and output root must be absolute paths "
                "(got \"%s\" and \"%s\")\n",
                argv[1], argv[2]);
        return CVF_CONSUMER_EXIT_USAGE;
    }

    {
        int exit_code = run_lifecycle(argv[1], argv[2]);

        printf("cvf package consumer: checks=%d failures=%d result=%s\n", g_checks, g_failures,
               g_failures == 0 ? "PASS" : "FAIL");
        return exit_code;
    }
}

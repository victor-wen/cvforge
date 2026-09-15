/*
 * cvforwin native C host example - TEMPLATE, NOT PRODUCTION INSPECTION CODE.
 *
 * This is the smallest realistic consumer of the frozen cvforwin C ABI v1. It
 * includes only <cvforwin/cvf_api.h> and needs no C++ knowledge: query the ABI
 * version, initialize one context with an absolute configuration root and an
 * absolute output root, run one cvf_inspect call per cycle (printing status,
 * verdict, elapsed time, an output-JSON excerpt, and the saved-image path),
 * reload the recipes atomically, and shut the context down.
 *
 * Every failure path returns a non-zero exit code; the example never treats a
 * technical error as a product verdict.
 *
 * Usage:
 *   cvf_c_host_example <absolute config root> <absolute output root> [cycles]
 *
 *   - config root: directory holding cvforwin.json and the recipes directory
 *   - output root: directory for logs/ and captures/ (created if absent)
 *   - cycles:      number of inspections to run (1..100, default 1)
 *
 * Exit codes:
 *   0  the whole lifecycle succeeded (all cycles OK)
 *   1  the library or an expectation failed (details printed)
 *   2  usage error (missing or invalid arguments)
 *
 * Local runs use examples/c_host/config, whose "synthetic" backend is accepted
 * only by a test-enabled build (CVFORWIN_BUILD_TEST_BACKENDS=ON), for example
 * the wsl-gcc-debug preset. A deployment host uses a "uvc" configuration like
 * config/examples/cvforwin.json from the release package.
 */

#include <cvforwin/cvf_api.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CVF_EXAMPLE_EXIT_OK = 0,
    CVF_EXAMPLE_EXIT_FAILED = 1,
    CVF_EXAMPLE_EXIT_USAGE = 2
};

enum {
    CVF_EXAMPLE_MAX_CYCLES = 100,
    CVF_EXAMPLE_OUTPUT_EXCERPT_BYTES = 160
};

static const char k_default_recipe_id[] = "example.pass";

static char g_output_json[CVF_RESULT_JSON_REQUIRED_CAPACITY];
static char g_image_path[CVF_IMAGE_PATH_REQUIRED_CAPACITY];
static char g_result_message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
static char g_error_message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];

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
    default: return "CVF_STATUS_UNKNOWN";
    }
}

static const char* verdict_name(cvf_verdict_t verdict)
{
    switch (verdict) {
    case CVF_VERDICT_NOT_EVALUATED: return "NOT_EVALUATED";
    case CVF_VERDICT_PASS: return "PASS";
    case CVF_VERDICT_FAIL: return "FAIL";
    default: return "UNKNOWN";
    }
}

static const char* level_name(uint32_t level)
{
    switch (level) {
    case CVF_LOG_LEVEL_TRACE: return "trace";
    case CVF_LOG_LEVEL_DEBUG: return "debug";
    case CVF_LOG_LEVEL_INFO: return "info";
    case CVF_LOG_LEVEL_WARN: return "warn";
    case CVF_LOG_LEVEL_ERROR: return "error";
    case CVF_LOG_LEVEL_CRITICAL: return "critical";
    default: return "unknown";
    }
}

static void log_callback(uint32_t level, const char* message_utf8, void* user_data)
{
    (void)user_data;
    printf("[cvforwin log level=%s] %s\n", level_name(level), message_utf8 != NULL ? message_utf8 : "");
}

static void report_error(const char* step, cvf_status_t status, const cvf_error_info_v1* error)
{
    printf("%s failed: status=%u (%s)", step, (unsigned)status, status_name(status));
    if (error != NULL && error->message_utf8 != NULL) {
        printf(" error_code=%u message=\"%s\"", (unsigned)error->error_code, error->message_utf8);
    }
    printf("\n");
    ++g_failures;
}

static void print_output_excerpt(const char* json)
{
    const size_t length = json != NULL ? strlen(json) : 0U;

    printf("  output_json: ");
    if (json == NULL || length == 0U) {
        printf("(empty)");
    } else if (length <= (size_t)CVF_EXAMPLE_OUTPUT_EXCERPT_BYTES) {
        printf("%s", json);
    } else {
        printf("%.*s... (%u bytes total)", (int)CVF_EXAMPLE_OUTPUT_EXCERPT_BYTES, json, (unsigned)length);
    }
    printf("\n");
}

static void make_options(cvf_init_options_v1* options, const char* config_root, const char* output_root)
{
    memset(options, 0, sizeof *options);
    options->struct_size = (uint32_t)sizeof *options;
    options->abi_version = CVF_ABI_VERSION_V1;
    options->config_root_utf8 = config_root;
    options->config_root_utf8_bytes = (uint32_t)strlen(config_root);
    options->output_root_utf8 = output_root;
    options->output_root_utf8_bytes = (uint32_t)strlen(output_root);
    options->flags = CVF_INIT_FLAG_FILE_LOGGING | CVF_INIT_FLAG_CALLBACK_LOGGING;
    options->log_callback = log_callback;
    options->user_data = NULL;
}

static void make_error(cvf_error_info_v1* error, char* buffer)
{
    memset(error, 0, sizeof *error);
    error->struct_size = (uint32_t)sizeof *error;
    error->message_utf8 = buffer;
    error->message_capacity = CVF_ERROR_MESSAGE_REQUIRED_CAPACITY;
}

static void make_request(cvf_inspection_request_v1* request, const char* recipe_id, const char* request_id)
{
    memset(request, 0, sizeof *request);
    request->struct_size = (uint32_t)sizeof *request;
    request->abi_version = CVF_ABI_VERSION_V1;
    request->recipe_id_utf8 = recipe_id;
    request->recipe_id_utf8_bytes = (uint32_t)strlen(recipe_id);
    request->request_id_utf8 = request_id;
    request->request_id_utf8_bytes = (uint32_t)strlen(request_id);
    request->timeout_ms = 0U; /* 0 selects the documented 5000 ms default. */
}

static void make_result(cvf_inspection_result_v1* result)
{
    memset(result, 0, sizeof *result);
    result->struct_size = (uint32_t)sizeof *result;
    result->output_json = g_output_json;
    result->output_json_capacity = (uint32_t)sizeof g_output_json;
    result->image_path = g_image_path;
    result->image_path_capacity = (uint32_t)sizeof g_image_path;
    result->error_message = g_result_message;
    result->error_message_capacity = (uint32_t)sizeof g_result_message;
}

static int run_cycles(cvf_context* context, const char* recipe_id, unsigned cycles)
{
    unsigned cycle = 0U;

    for (cycle = 1U; cycle <= cycles; ++cycle) {
        cvf_inspection_request_v1 request;
        cvf_inspection_result_v1 result;
        char request_id[64];
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        (void)snprintf(request_id, sizeof request_id, "example-cycle-%u", cycle);
        make_request(&request, recipe_id, request_id);
        make_result(&result);

        status = cvf_inspect(context, &request, &result);
        printf("cycle %u: recipe=\"%s\" request=\"%s\"\n", cycle, recipe_id, request_id);
        printf("  status=%u (%s) verdict=%s elapsed_ms=%u error_code=%u warning_flags=0x%08x\n",
               (unsigned)status, status_name(status), verdict_name(result.verdict),
               (unsigned)result.elapsed_ms, (unsigned)result.error_code, (unsigned)result.warning_flags);
        print_output_excerpt(result.output_json);
        printf("  image_path: %s\n",
               result.image_path_bytes_written > 0U ? result.image_path : "(none)");

        if (status != CVF_STATUS_OK) {
            report_error("cvf_inspect", status, NULL);
            printf("  result.error_message: %s\n",
                   result.error_message_bytes_written > 0U ? result.error_message : "(none)");
            if (result.verdict != CVF_VERDICT_NOT_EVALUATED) {
                printf("  ERROR: a failed inspection must report NOT_EVALUATED, not %s\n",
                       verdict_name(result.verdict));
                ++g_failures;
            }
        }
    }
    return g_failures == 0 ? CVF_EXAMPLE_EXIT_OK : CVF_EXAMPLE_EXIT_FAILED;
}

static int parse_cycles(const char* text, unsigned* out_cycles)
{
    char* end = NULL;
    unsigned long value = strtoul(text, &end, 10);

    if (end == text || *end != '\0' || value < 1UL || value > (unsigned long)CVF_EXAMPLE_MAX_CYCLES) {
        return 0;
    }
    *out_cycles = (unsigned)value;
    return 1;
}

int main(int argc, char** argv)
{
    const char* config_root = NULL;
    const char* output_root = NULL;
    const char* recipe_id = k_default_recipe_id;
    unsigned cycles = 1U;
    cvf_init_options_v1 options;
    cvf_error_info_v1 error;
    cvf_context* context = NULL;
    cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;
    int exit_code = CVF_EXAMPLE_EXIT_FAILED;

    if (argc < 3 || argc > 4) {
        fprintf(stderr,
                "usage: %s <absolute config root> <absolute output root> [cycles]\n"
                "  cycles is optional (1..%d, default 1)\n",
                argv[0] != NULL ? argv[0] : "cvf_c_host_example", CVF_EXAMPLE_MAX_CYCLES);
        return CVF_EXAMPLE_EXIT_USAGE;
    }
    config_root = argv[1];
    output_root = argv[2];
    if (argc == 4 && !parse_cycles(argv[3], &cycles)) {
        fprintf(stderr, "usage error: cycles must be an integer in 1..%d, got \"%s\"\n",
                CVF_EXAMPLE_MAX_CYCLES, argv[3]);
        return CVF_EXAMPLE_EXIT_USAGE;
    }

    printf("cvforwin native C host example (template; not a production inspection)\n");
    printf("config_root=\"%s\" output_root=\"%s\" recipe=\"%s\" cycles=%u\n", config_root,
           output_root, recipe_id, cycles);

    /* The ABI version is available before any context exists. */
    {
        const uint32_t abi_version = cvf_get_abi_version();

        printf("cvf_get_abi_version()=%u (expected %u)\n", (unsigned)abi_version,
               (unsigned)CVF_ABI_VERSION_V1);
        if (abi_version != CVF_ABI_VERSION_V1) {
            printf("ERROR: ABI version mismatch\n");
            return CVF_EXAMPLE_EXIT_FAILED;
        }
    }

    make_error(&error, g_error_message);
    make_options(&options, config_root, output_root);

    status = cvf_initialize(&options, &context, &error);
    if (status != CVF_STATUS_OK || context == NULL) {
        report_error("cvf_initialize", status, &error);
        return CVF_EXAMPLE_EXIT_FAILED;
    }
    printf("cvf_initialize: OK\n");

    exit_code = run_cycles(context, recipe_id, cycles);

    make_error(&error, g_error_message);
    status = cvf_reload_recipes(context, &error);
    if (status != CVF_STATUS_OK) {
        report_error("cvf_reload_recipes", status, &error);
    } else {
        printf("cvf_reload_recipes: OK\n");
    }

    status = cvf_shutdown(context);
    if (status != CVF_STATUS_OK) {
        report_error("cvf_shutdown", status, NULL);
    } else {
        printf("cvf_shutdown: OK\n");
    }

    printf("cvforwin C host example: %s (failures=%d)\n",
           g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? exit_code : CVF_EXAMPLE_EXIT_FAILED;
}

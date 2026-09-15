/*
 * CVF-006 independent black-box test - brief B1-B6 and B13.
 *
 * Full public C ABI lifecycle of the runtime orchestrator:
 *   B1  initialize happy path, single-context limit, shutdown, re-initialize;
 *   B2  initialize failures and v1 error-info buffer rules;
 *   B3  inspect happy path, verdict/measurement/byte-count rules;
 *   B4  inspect validation, undersized buffers, unknown recipe;
 *   B5  atomic recipe reload with old-snapshot preservation;
 *   B6  serialized concurrent inspect calls on one context;
 *   B13 request identifier and input_json validation plus input passthrough.
 *
 * Authored by test-engineer from .ai/test-briefs/CVF-006.yaml and the approved
 * .ai/project-contract.yaml only; no production implementation was consulted.
 *
 * Build inputs required from CMake (test-only compile definitions):
 *   CVF_TEST_FIXTURES_DIR - read-only fixtures root
 *                           (<dir>/valid_config is a complete valid config root)
 *   CVF_TEST_TMP_DIR      - writable scratch root
 *
 * RED / verify commands:
 *   gcc -std=c11 -Wall -Wextra -Werror -I include \
 *       -DCVF_TEST_FIXTURES_DIR='"tests/fixtures/runtime"' \
 *       -DCVF_TEST_TMP_DIR='"/tmp/cvf006"' -fsyntax-only tests/abi/cvf006_lifecycle.c
 *   link this file against the cvforwin runtime and run the produced executable.
 *
 * The suite is deterministic and hardware-free: configurations, recipes and
 * frames are generated at run time with plain C file I/O below CVF_TEST_TMP_DIR,
 * and the only camera sources are the runtime's test-enabled synthetic and file
 * backends. A crash aborts the run, which is itself a failure.
 */
#include <cvforwin/cvf_api.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef CVF_TEST_FIXTURES_DIR
#error "CVF_TEST_FIXTURES_DIR must be defined by the build for the CVF-006 C lifecycle test"
#endif
#ifndef CVF_TEST_TMP_DIR
#error "CVF_TEST_TMP_DIR must be defined by the build for the CVF-006 C lifecycle test"
#endif

#define CVF_FIXTURE_CONFIG_ROOT CVF_TEST_FIXTURES_DIR "/valid_config"

static const char kPassParameters[] = "{\"threshold\": 128, \"min_pass_ratio\": 0.0}";
static const char kFailParameters[] = "{\"threshold\": 255, \"min_pass_ratio\": 1.0}";
/* All-white passes with ratio 1.0 and inverting it yields 0.0 -> FAIL. */
static const char kFlipParameters[] = "{\"threshold\": 128, \"min_pass_ratio\": 0.5}";

static const char kSyntheticConfig[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"camera\": {\n"
    "    \"backend\": \"synthetic\",\n"
    "    \"device_path\": \"synthetic0\",\n"
    "    \"vendor_id\": \"0000\",\n"
    "    \"product_id\": \"0000\",\n"
    "    \"friendly_name\": \"Synthetic camera\"\n"
    "  },\n"
    "  \"base_capture\": {\"width\": 16, \"height\": 12, \"frame_rate\": 30.0, \"pixel_format\": "
    "\"bgr8\"},\n"
    "  \"logging\": {\"level\": \"info\", \"max_file_bytes\": 1048576, \"max_files\": 2},\n"
    "  \"retention\": {\"max_age_days\": 30, \"max_total_bytes\": 1073741824}\n"
    "}\n";

/* %s is the absolute frames directory (the file backend's device_path). */
static const char kFileConfigTemplate[] =
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"camera\": {\n"
    "    \"backend\": \"file\",\n"
    "    \"device_path\": \"%s\",\n"
    "    \"vendor_id\": \"0000\",\n"
    "    \"product_id\": \"0000\",\n"
    "    \"friendly_name\": \"File camera\"\n"
    "  },\n"
    "  \"base_capture\": {\"width\": 16, \"height\": 12, \"frame_rate\": 30.0, \"pixel_format\": "
    "\"bgr8\"},\n"
    "  \"logging\": {\"level\": \"info\", \"max_file_bytes\": 1048576, \"max_files\": 2},\n"
    "  \"retention\": {\"max_age_days\": 30, \"max_total_bytes\": 1073741824}\n"
    "}\n";

static char g_output_json[CVF_RESULT_JSON_REQUIRED_CAPACITY];
static char g_image_path[CVF_IMAGE_PATH_REQUIRED_CAPACITY];
static char g_result_error_message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
static char g_base[1024];
static char g_fixture_config_root[1024];

static int g_checks = 0;
static int g_failures = 0;

/* ------------------------------------------------------------------------- */
/* reporting                                                                  */
/* ------------------------------------------------------------------------- */

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
        printf("FAIL [%s] expected status %u (%s), observed %u (%s)\n", test_name,
               (unsigned)expected, status_name(expected), (unsigned)observed,
               status_name(observed));
    } else {
        printf("ok   [%s] status=%u (%s)\n", test_name, (unsigned)observed,
               status_name(observed));
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

static void check_u32(const char* test_name, uint32_t observed, uint32_t expected,
                      const char* what)
{
    ++g_checks;
    if (observed != expected) {
        ++g_failures;
        printf("FAIL [%s] expected %s = %u, observed %u\n", test_name, what,
               (unsigned)expected, (unsigned)observed);
    } else {
        printf("ok   [%s] %s = %u\n", test_name, what, (unsigned)observed);
    }
}

/* ------------------------------------------------------------------------- */
/* plain C file helpers                                                       */
/* ------------------------------------------------------------------------- */

static int make_dirs(const char* path)
{
    char buffer[1024];
    size_t length = 0;
    size_t index = 0;

    if (path == NULL) {
        return 0;
    }
    length = strlen(path);
    if (length == 0u || length >= sizeof buffer) {
        return 0;
    }
    memcpy(buffer, path, length + 1u);
    for (index = 1u; index <= length; ++index) {
        if (buffer[index] == '/' || buffer[index] == '\0') {
            const char saved = buffer[index];
            buffer[index] = '\0';
#ifdef _WIN32
            if (_mkdir(buffer) != 0 && errno != EEXIST) {
                return 0;
            }
#else
            if (mkdir(buffer, 0700) != 0 && errno != EEXIST) {
                return 0;
            }
#endif
            buffer[index] = saved;
        }
    }
    return 1;
}

static void join_path(char* out, size_t size, const char* first, const char* second)
{
    snprintf(out, size, "%s/%s", first, second);
}

/* Plain concatenation with a hard bound; avoids GCC -Wformat-truncation on
 * constant-size buffers while keeping the result NUL-terminated. */
static void concat_text(char* out, size_t size, const char* first, const char* second)
{
    size_t first_length = strlen(first);
    size_t second_length = strlen(second);
    size_t total = first_length + second_length;

    if (total >= size) {
        total = size - 1u;
    }
    if (first_length > total) {
        first_length = total;
    }
    memcpy(out, first, first_length);
    if (total > first_length) {
        memcpy(out + first_length, second, total - first_length);
    }
    out[total] = '\0';
}

static int is_absolute_path(const char* path)
{
#ifdef _WIN32
    return path[0] == '/' || path[0] == '\\' || (path[0] != '\0' && path[1] == ':');
#else
    return path[0] == '/';
#endif
}

/* The contract requires absolute caller-provided roots, so make sure the
 * CMake-provided fixture and scratch roots are absolute before use. */
static void make_absolute_path(const char* path, char* out, size_t size)
{
    char cwd[1024];

    if (is_absolute_path(path)) {
        snprintf(out, size, "%s", path);
        return;
    }
#ifdef _WIN32
    if (_getcwd(cwd, (int)sizeof cwd) == NULL) {
#else
    if (getcwd(cwd, sizeof cwd) == NULL) {
#endif
        snprintf(out, size, "%s", path);
        return;
    }
    snprintf(out, size, "%s/%s", cwd, path);
}

static int write_text_file(const char* path, const char* text)
{
    FILE* file = fopen(path, "wb");
    size_t length = strlen(text);
    int ok = 0;

    if (file == NULL) {
        return 0;
    }
    ok = fwrite(text, 1u, length, file) == length;
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

static int write_file_in_dir(const char* dir, const char* name, const char* text)
{
    char path[1024];
    join_path(path, sizeof path, dir, name);
    return write_text_file(path, text);
}

static int remove_file_in_dir(const char* dir, const char* name)
{
    char path[1024];
    join_path(path, sizeof path, dir, name);
    return remove(path) == 0;
}

static int remove_recipe(const char* config_dir, const char* filename)
{
    char recipes_dir[1024];
    join_path(recipes_dir, sizeof recipes_dir, config_dir, "recipes");
    return remove_file_in_dir(recipes_dir, filename);
}

static uint32_t text_bytes(const char* text)
{
    return (uint32_t)strlen(text);
}

static int write_ppm_p6(const char* dir, const char* name, int width, int height,
                        unsigned char value)
{
    char path[1024];
    FILE* file = NULL;
    int ok = 0;
    int index = 0;

    join_path(path, sizeof path, dir, name);
    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    ok = fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
    for (index = 0; ok && index < width * height; ++index) {
        const unsigned char pixel[3] = {value, value, value};
        ok = fwrite(pixel, 1u, sizeof pixel, file) == sizeof pixel;
    }
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

/* Mixed frame: the first white_pixels pixels are white (255) and the rest are
 * black (0), so pass_ratio is exactly white_pixels / (width * height). */
static int write_ppm_p6_mixed(const char* dir, const char* name, int width, int height,
                              int white_pixels)
{
    char path[1024];
    FILE* file = NULL;
    int ok = 0;
    int index = 0;
    const int total = width * height;

    join_path(path, sizeof path, dir, name);
    file = fopen(path, "wb");
    if (file == NULL) {
        return 0;
    }
    ok = fprintf(file, "P6\n%d %d\n255\n", width, height) > 0;
    for (index = 0; ok && index < total; ++index) {
        const unsigned char value = index < white_pixels ? 255u : 0u;
        const unsigned char pixel[3] = {value, value, value};
        ok = fwrite(pixel, 1u, sizeof pixel, file) == sizeof pixel;
    }
    if (fclose(file) != 0) {
        ok = 0;
    }
    return ok;
}

static void make_case_dir(char* out, size_t size, const char* name)
{
    join_path(out, size, g_base, name);
    (void)make_dirs(out);
}

static int json_contains_key(const char* text, const char* key)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\"", key);
    return strstr(text, needle) != NULL;
}

/* Returns the integer value after "key": in a JSON object text, or -1. */
static long json_number_after(const char* text, const char* key)
{
    char needle[64];
    const char* at = NULL;

    snprintf(needle, sizeof needle, "\"%s\"", key);
    at = strstr(text, needle);
    if (at == NULL) {
        return -1;
    }
    at += strlen(needle);
    while (*at == ' ' || *at == '\t') {
        ++at;
    }
    if (*at != ':') {
        return -1;
    }
    ++at;
    while (*at == ' ' || *at == '\t') {
        ++at;
    }
    if (*at < '0' || *at > '9') {
        return -1;
    }
    return strtol(at, NULL, 10);
}

/* ------------------------------------------------------------------------- */
/* fixtures and call helpers                                                  */
/* ------------------------------------------------------------------------- */

static int write_synthetic_config(const char* config_dir)
{
    if (!make_dirs(config_dir)) {
        return 0;
    }
    return write_file_in_dir(config_dir, "cvforwin.json", kSyntheticConfig);
}

static int write_file_config(const char* config_dir, const char* frames_dir)
{
    char text[2048];
    if (!make_dirs(config_dir)) {
        return 0;
    }
    snprintf(text, sizeof text, kFileConfigTemplate, frames_dir);
    return write_file_in_dir(config_dir, "cvforwin.json", text);
}

static int add_recipe(const char* config_dir, const char* filename, const char* recipe_id,
                      const char* algorithm, const char* parameters)
{
    char recipes_dir[1024];
    char text[2048];

    join_path(recipes_dir, sizeof recipes_dir, config_dir, "recipes");
    if (!make_dirs(recipes_dir)) {
        return 0;
    }
    snprintf(text, sizeof text,
             "{\n"
             "  \"schema_version\": 1,\n"
             "  \"recipe_id\": \"%s\",\n"
             "  \"algorithm\": \"%s\",\n"
             "  \"parameters\": %s,\n"
             "  \"capture\": {\"settle_frames\": 0},\n"
             "  \"artifacts\": {\"save_policy\": \"never\", \"required\": false}\n"
             "}\n",
             recipe_id, algorithm, parameters);
    return write_file_in_dir(recipes_dir, filename, text);
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

static cvf_inspection_request_v1 make_request_span(const char* recipe_id, uint32_t recipe_bytes,
                                                   const char* request_id, uint32_t request_bytes,
                                                   uint32_t timeout_ms, const char* input_json,
                                                   uint32_t input_bytes)
{
    cvf_inspection_request_v1 request;

    memset(&request, 0, sizeof request);
    request.struct_size = (uint32_t)sizeof request;
    request.abi_version = CVF_ABI_VERSION_V1;
    request.recipe_id_utf8 = recipe_id;
    request.recipe_id_utf8_bytes = recipe_bytes;
    request.request_id_utf8 = request_id;
    request.request_id_utf8_bytes = request_bytes;
    request.timeout_ms = timeout_ms;
    request.input_json_utf8 = input_json;
    request.input_json_utf8_bytes = input_bytes;
    return request;
}

static cvf_inspection_request_v1 make_request(const char* recipe_id, const char* request_id)
{
    return make_request_span(recipe_id, (uint32_t)strlen(recipe_id), request_id,
                             (uint32_t)strlen(request_id), 0u, NULL, 0u);
}

static cvf_inspection_result_v1 make_result_caps(uint32_t json_capacity, uint32_t path_capacity,
                                                 uint32_t message_capacity)
{
    cvf_inspection_result_v1 result;

    memset(&result, 0, sizeof result);
    result.struct_size = (uint32_t)sizeof result;
    result.output_json = g_output_json;
    result.output_json_capacity = json_capacity;
    result.image_path = g_image_path;
    result.image_path_capacity = path_capacity;
    result.error_message = g_result_error_message;
    result.error_message_capacity = message_capacity;
    return result;
}

static cvf_inspection_result_v1 make_result_full(void)
{
    return make_result_caps((uint32_t)sizeof g_output_json, (uint32_t)sizeof g_image_path,
                            (uint32_t)sizeof g_result_error_message);
}

static void reset_result_buffers(void)
{
    memset(g_output_json, 'X', sizeof g_output_json);
    memset(g_image_path, 'X', sizeof g_image_path);
    memset(g_result_error_message, 'X', sizeof g_result_error_message);
}

static int initialize_ok(const char* test_name, const char* config_root, const char* output_root,
                         cvf_context** out_context)
{
    char message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
    cvf_error_info_v1 error = make_error(message, (uint32_t)sizeof message);
    cvf_init_options_v1 options = make_options(config_root, output_root);
    cvf_status_t status = cvf_initialize(&options, out_context, &error);

    check_status(test_name, status, CVF_STATUS_OK);
    check_condition(test_name, *out_context != NULL,
                    "out_context is non-null after a successful cvf_initialize");
    return status == CVF_STATUS_OK && *out_context != NULL;
}

static void check_error_reported(const char* test_name, const cvf_error_info_v1* error,
                                 const char* buffer)
{
    check_condition(test_name, error->error_code != 0u,
                    "error_code is nonzero after a rejected call");
    check_condition(test_name, error->message_bytes_written <= error->message_capacity,
                    "message_bytes_written never exceeds message_capacity");
    check_condition(test_name, (size_t)error->message_bytes_written < (size_t)error->message_capacity,
                    "message_bytes_written stays inside the caller buffer");
    check_condition(test_name, buffer[error->message_bytes_written] == '\0',
                    "the error message is NUL-terminated at message_bytes_written");
    check_condition(test_name, error->message_bytes_required >= error->message_bytes_written + 1u,
                    "message_bytes_required includes the trailing NUL");
}

static void expect_initialize_status(const char* test_name, const char* config_root,
                                     const char* output_root, cvf_status_t expected)
{
    char message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
    cvf_error_info_v1 error = make_error(message, (uint32_t)sizeof message);
    cvf_init_options_v1 options = make_options(config_root, output_root);
    cvf_context* context = NULL;
    cvf_status_t status = cvf_initialize(&options, &context, &error);

    check_status(test_name, status, expected);
    check_condition(test_name, context == NULL,
                    "out_context stays NULL when cvf_initialize rejects the call");
    check_error_reported(test_name, &error, message);
}

/* ------------------------------------------------------------------------- */
/* B1: initialize happy path, single context, shutdown, re-initialize         */
/* ------------------------------------------------------------------------- */

static void case_b1_lifecycle(void)
{
    char output_root[1024];
    cvf_context* first = NULL;
    cvf_context* third = NULL;

    make_case_dir(output_root, sizeof output_root, "b1_output");

    if (!initialize_ok("b1_initialize_with_fixture_config", g_fixture_config_root, output_root,
                       &first)) {
        return;
    }

    {
        char message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
        cvf_error_info_v1 error = make_error(message, (uint32_t)sizeof message);
        cvf_init_options_v1 options = make_options(g_fixture_config_root, output_root);
        cvf_context* second = NULL;
        cvf_status_t status = cvf_initialize(&options, &second, &error);

        check_status("b1_second_live_context_limit", status, CVF_STATUS_CONTEXT_LIMIT);
        check_condition("b1_second_live_context_limit", second == NULL,
                        "out_context stays NULL when the single-context limit is hit");
    }

    check_status("b1_shutdown_ok", cvf_shutdown(first), CVF_STATUS_OK);
    check_status("b1_shutdown_null_invalid_context", cvf_shutdown(NULL), CVF_STATUS_INVALID_CONTEXT);

    if (initialize_ok("b1_initialize_after_shutdown", g_fixture_config_root, output_root, &third)) {
        cvf_inspection_request_v1 request = make_request("example", "b1-new-context");
        cvf_inspection_result_v1 result;
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        reset_result_buffers();
        result = make_result_full();
        status = cvf_inspect(third, &request, &result);
        check_status("b1_new_context_is_functional", status, CVF_STATUS_OK);
        check_u32("b1_new_context_is_functional", (uint32_t)result.verdict, CVF_VERDICT_PASS,
                  "verdict of the re-initialized context");
        check_status("b1_shutdown_second_ok", cvf_shutdown(third), CVF_STATUS_OK);
    }
}

/* ------------------------------------------------------------------------- */
/* B2: initialize failures and error-info rules                               */
/* ------------------------------------------------------------------------- */

static void case_b2_initialize_failures(void)
{
    char case_root[1024];
    char output_root[1024];
    char missing_config[1024];
    char empty_config[1024];
    char invalid_config[1024];
    char unknown_algo[1024];
    char duplicate_ids[1024];
    char valid_config[1024];

    make_case_dir(case_root, sizeof case_root, "b2");
    make_case_dir(output_root, sizeof output_root, "b2_output");

    join_path(missing_config, sizeof missing_config, case_root, "missing_config");
    expect_initialize_status("b2_missing_config_root", missing_config, output_root,
                             CVF_STATUS_CONFIG_ERROR);

    join_path(empty_config, sizeof empty_config, case_root, "empty_config");
    (void)make_dirs(empty_config);
    expect_initialize_status("b2_missing_config_file", empty_config, output_root,
                             CVF_STATUS_CONFIG_ERROR);

    join_path(invalid_config, sizeof invalid_config, case_root, "invalid_config");
    (void)make_dirs(invalid_config);
    (void)write_file_in_dir(invalid_config, "cvforwin.json", "{ this is not valid JSON");
    expect_initialize_status("b2_invalid_config_json", invalid_config, output_root,
                             CVF_STATUS_CONFIG_ERROR);

    join_path(unknown_algo, sizeof unknown_algo, case_root, "unknown_algo");
    (void)write_synthetic_config(unknown_algo);
    (void)add_recipe(unknown_algo, "bad.json", "bad.recipe", "no.such.algorithm", kPassParameters);
    expect_initialize_status("b2_unknown_recipe_algorithm", unknown_algo, output_root,
                             CVF_STATUS_CONFIG_ERROR);

    join_path(duplicate_ids, sizeof duplicate_ids, case_root, "duplicate_ids");
    (void)write_synthetic_config(duplicate_ids);
    (void)add_recipe(duplicate_ids, "one.json", "duplicate.recipe", "example.threshold",
                     kPassParameters);
    (void)add_recipe(duplicate_ids, "two.json", "duplicate.recipe", "example.threshold",
                     kPassParameters);
    expect_initialize_status("b2_duplicate_recipe_id", duplicate_ids, output_root,
                             CVF_STATUS_CONFIG_ERROR);

    /* A failed initialize must not leave a context slot behind. */
    join_path(valid_config, sizeof valid_config, case_root, "valid_config");
    (void)write_synthetic_config(valid_config);
    (void)add_recipe(valid_config, "example.json", "example", "example.threshold", kPassParameters);
    {
        cvf_context* context = NULL;
        if (initialize_ok("b2_retry_after_failed_initialize", valid_config, output_root, &context)) {
            check_status("b2_retry_after_failed_initialize", cvf_shutdown(context), CVF_STATUS_OK);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* B3: inspect happy path                                                     */
/* ------------------------------------------------------------------------- */

static void check_inspect_success(const char* test_name, cvf_status_t status,
                                  cvf_inspection_result_v1* result, uint32_t expected_verdict)
{
    check_status(test_name, status, CVF_STATUS_OK);
    check_u32(test_name, (uint32_t)result->status, CVF_STATUS_OK, "result.status");
    check_u32(test_name, (uint32_t)result->verdict, expected_verdict, "result.verdict");
    check_u32(test_name, result->error_code, 0u, "result.error_code");
    check_condition(test_name, (uint64_t)result->elapsed_ms < 30000u,
                    "result.elapsed_ms is a sane non-negative duration");
    check_condition(test_name, result->output_json_bytes_written > 0u,
                    "output_json_bytes_written is nonzero on success");
    check_condition(test_name, result->output_json_bytes_written < result->output_json_capacity,
                    "output_json_bytes_written excludes the trailing NUL");
    check_condition(test_name, result->output_json[result->output_json_bytes_written] == '\0',
                    "output_json is NUL-terminated at bytes_written");
    check_condition(test_name,
                    result->output_json_bytes_required == result->output_json_bytes_written + 1u,
                    "output_json_bytes_required includes the trailing NUL");
    check_condition(test_name, result->output_json[0] == '{',
                    "output_json is a UTF-8 JSON object");
    check_condition(test_name,
                    json_contains_key(result->output_json, "white_pixels") &&
                        json_contains_key(result->output_json, "total_pixels") &&
                        json_contains_key(result->output_json, "pass_ratio"),
                    "output_json contains the example algorithm measurements");
}

static void check_no_saved_image(const char* test_name, const cvf_inspection_result_v1* result)
{
    check_u32(test_name, result->image_path_bytes_written, 0u, "image_path_bytes_written");
    check_condition(test_name, result->image_path[0] == '\0',
                    "image_path is cleared when nothing is saved");
}

static void case_b3_inspect_happy_path(void)
{
    char config_root[1024];
    char output_root[1024];
    cvf_context* context = NULL;

    make_case_dir(config_root, sizeof config_root, "b3_config");
    make_case_dir(output_root, sizeof output_root, "b3_output");
    (void)write_synthetic_config(config_root);
    (void)add_recipe(config_root, "pass.json", "example.pass", "example.threshold", kPassParameters);
    (void)add_recipe(config_root, "fail.json", "example.fail", "example.threshold", kFailParameters);

    if (!initialize_ok("b3_initialize", config_root, output_root, &context)) {
        return;
    }

    {
        cvf_inspection_request_v1 request = make_request("example.pass", "b3-pass");
        cvf_inspection_result_v1 result;
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        reset_result_buffers();
        result = make_result_full();
        status = cvf_inspect(context, &request, &result);
        check_inspect_success("b3_pass", status, &result, CVF_VERDICT_PASS);
        check_no_saved_image("b3_pass", &result);
    }

    {
        cvf_inspection_request_v1 request = make_request("example.fail", "b3-fail");
        cvf_inspection_result_v1 result;
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        reset_result_buffers();
        result = make_result_full();
        status = cvf_inspect(context, &request, &result);
        check_inspect_success("b3_fail", status, &result, CVF_VERDICT_FAIL);
    }

    check_status("b3_shutdown", cvf_shutdown(context), CVF_STATUS_OK);
}

/*
 * Brief boundary case: "verdict boundary pass_ratio == min_pass_ratio -> PASS".
 * A 4x4 frame with 8 pure-white and 8 pure-black pixels has pass_ratio exactly
 * 0.5, so min_pass_ratio 0.5 must PASS while a value just above (0.51) must FAIL.
 */
static void case_b3_verdict_ratio_boundary(void)
{
    char config_root[1024];
    char output_root[1024];
    char frames_dir[1024];
    cvf_context* context = NULL;

    make_case_dir(frames_dir, sizeof frames_dir, "b3_ratio_frames");
    (void)write_ppm_p6_mixed(frames_dir, "frame0.ppm", 4, 4, 8);
    (void)write_ppm_p6_mixed(frames_dir, "frame1.ppm", 4, 4, 8);

    make_case_dir(config_root, sizeof config_root, "b3_ratio_config");
    make_case_dir(output_root, sizeof output_root, "b3_ratio_output");
    (void)write_file_config(config_root, frames_dir);
    (void)add_recipe(config_root, "equal.json", "example.ratio.equal", "example.threshold",
                     "{\"threshold\": 128, \"min_pass_ratio\": 0.5}");
    (void)add_recipe(config_root, "above.json", "example.ratio.above", "example.threshold",
                     "{\"threshold\": 128, \"min_pass_ratio\": 0.51}");

    if (!initialize_ok("b3_ratio_initialize", config_root, output_root, &context)) {
        return;
    }

    {
        cvf_inspection_request_v1 request = make_request("example.ratio.equal", "b3-ratio-equal");
        cvf_inspection_result_v1 result;
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        reset_result_buffers();
        result = make_result_full();
        status = cvf_inspect(context, &request, &result);
        check_status("b3_ratio_equal_passes", status, CVF_STATUS_OK);
        check_u32("b3_ratio_equal_passes", (uint32_t)result.verdict, CVF_VERDICT_PASS,
                  "verdict when pass_ratio equals min_pass_ratio");
        check_condition("b3_ratio_equal_passes",
                        json_number_after(result.output_json, "white_pixels") == 8 &&
                            json_number_after(result.output_json, "total_pixels") == 16,
                        "8 of 16 pixels are white so pass_ratio is exactly 0.5");
    }
    {
        cvf_inspection_request_v1 request = make_request("example.ratio.above", "b3-ratio-above");
        cvf_inspection_result_v1 result;
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        reset_result_buffers();
        result = make_result_full();
        status = cvf_inspect(context, &request, &result);
        check_status("b3_ratio_above_fails", status, CVF_STATUS_OK);
        check_u32("b3_ratio_above_fails", (uint32_t)result.verdict, CVF_VERDICT_FAIL,
                  "verdict when pass_ratio is just below min_pass_ratio");
    }

    check_status("b3_ratio_shutdown", cvf_shutdown(context), CVF_STATUS_OK);
}

/* ------------------------------------------------------------------------- */
/* B4: inspect validation, undersized buffers, unknown recipe                 */
/* ------------------------------------------------------------------------- */

static void case_b4_validation_and_errors(void)
{
    char config_root[1024];
    char output_root[1024];
    cvf_context* context = NULL;

    make_case_dir(config_root, sizeof config_root, "b4_config");
    make_case_dir(output_root, sizeof output_root, "b4_output");
    (void)write_synthetic_config(config_root);
    (void)add_recipe(config_root, "pass.json", "example.pass", "example.threshold", kPassParameters);

    /* Null context first: it needs no initialized context. */
    {
        cvf_inspection_request_v1 request = make_request("example.pass", "b4-null-context");
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b4_null_context", cvf_inspect(NULL, &request, &result),
                     CVF_STATUS_INVALID_CONTEXT);
        check_u32("b4_null_context", (uint32_t)result.verdict, CVF_VERDICT_NOT_EVALUATED,
                  "verdict after a non-OK inspect");
    }

    if (!initialize_ok("b4_initialize", config_root, output_root, &context)) {
        return;
    }

    {
        cvf_inspection_request_v1 request = make_request("example.pass", "b4-null-request");
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b4_null_request", cvf_inspect(context, NULL, &result),
                     CVF_STATUS_INVALID_ARGUMENT);
        check_status("b4_null_result", cvf_inspect(context, &request, NULL),
                     CVF_STATUS_INVALID_ARGUMENT);
    }

    {
        cvf_inspection_request_v1 request = make_request("example.pass", "b4-bad-request");
        cvf_inspection_result_v1 result = make_result_full();
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        request.struct_size = 0u;
        status = cvf_inspect(context, &request, &result);
        check_status("b4_request_struct_size_zero", status, CVF_STATUS_INVALID_ARGUMENT);

        request = make_request("example.pass", "b4-bad-request");
        request.struct_size = (uint32_t)sizeof request - 1u;
        status = cvf_inspect(context, &request, &result);
        check_status("b4_request_struct_size_minus_one", status, CVF_STATUS_INVALID_ARGUMENT);

        request = make_request("example.pass", "b4-bad-request");
        request.abi_version = 0u;
        status = cvf_inspect(context, &request, &result);
        check_status("b4_request_abi_version_zero", status, CVF_STATUS_ABI_MISMATCH);

        request = make_request("example.pass", "b4-bad-request");
        request.abi_version = 2u;
        status = cvf_inspect(context, &request, &result);
        check_status("b4_request_abi_version_two", status, CVF_STATUS_ABI_MISMATCH);

        request = make_request("example.pass", "b4-bad-request");
        memset(request.reserved, 0xFF, sizeof request.reserved);
        status = cvf_inspect(context, &request, &result);
        check_status("b4_request_reserved_nonzero", status, CVF_STATUS_INVALID_ARGUMENT);
    }

    {
        cvf_inspection_request_v1 request = make_request("example.pass", "b4-bad-result");
        cvf_inspection_result_v1 result = make_result_full();
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        result.struct_size = 0u;
        status = cvf_inspect(context, &request, &result);
        check_status("b4_result_struct_size_zero", status, CVF_STATUS_INVALID_ARGUMENT);

        result = make_result_full();
        result.struct_size = (uint32_t)sizeof result - 1u;
        status = cvf_inspect(context, &request, &result);
        check_status("b4_result_struct_size_minus_one", status, CVF_STATUS_INVALID_ARGUMENT);

        result = make_result_full();
        memset(result.reserved, 0xFF, sizeof result.reserved);
        status = cvf_inspect(context, &request, &result);
        check_status("b4_result_reserved_nonzero", status, CVF_STATUS_INVALID_ARGUMENT);
    }

    /* Undersized buffers are rejected before acquisition with bytes_required. */
    {
        cvf_inspection_request_v1 request = make_request("example.pass", "b4-small-json");
        cvf_inspection_result_v1 result = make_result_caps(
            CVF_RESULT_JSON_REQUIRED_CAPACITY - 1u, CVF_IMAGE_PATH_REQUIRED_CAPACITY,
            CVF_ERROR_MESSAGE_REQUIRED_CAPACITY);

        reset_result_buffers();
        check_status("b4_output_json_undersized", cvf_inspect(context, &request, &result),
                     CVF_STATUS_BUFFER_TOO_SMALL);
        check_u32("b4_output_json_undersized", result.output_json_bytes_written, 0u,
                  "output_json_bytes_written after rejection");
        check_condition("b4_output_json_undersized",
                        result.output_json_bytes_required >= CVF_RESULT_JSON_REQUIRED_CAPACITY,
                        "output_json_bytes_required reports the required capacity");
    }
    {
        cvf_inspection_request_v1 request = make_request("example.pass", "b4-small-path");
        cvf_inspection_result_v1 result = make_result_caps(
            CVF_RESULT_JSON_REQUIRED_CAPACITY, CVF_IMAGE_PATH_REQUIRED_CAPACITY - 1u,
            CVF_ERROR_MESSAGE_REQUIRED_CAPACITY);

        reset_result_buffers();
        check_status("b4_image_path_undersized", cvf_inspect(context, &request, &result),
                     CVF_STATUS_BUFFER_TOO_SMALL);
        check_condition("b4_image_path_undersized",
                        result.image_path_bytes_required >= CVF_IMAGE_PATH_REQUIRED_CAPACITY,
                        "image_path_bytes_required reports the required capacity");
    }
    {
        cvf_inspection_request_v1 request = make_request("example.pass", "b4-small-message");
        cvf_inspection_result_v1 result = make_result_caps(
            CVF_RESULT_JSON_REQUIRED_CAPACITY, CVF_IMAGE_PATH_REQUIRED_CAPACITY,
            CVF_ERROR_MESSAGE_REQUIRED_CAPACITY - 1u);

        reset_result_buffers();
        check_status("b4_error_message_undersized", cvf_inspect(context, &request, &result),
                     CVF_STATUS_BUFFER_TOO_SMALL);
        check_condition("b4_error_message_undersized",
                        result.error_message_bytes_required >= CVF_ERROR_MESSAGE_REQUIRED_CAPACITY,
                        "error_message_bytes_required reports the required capacity");
    }

    /* Unknown recipe: technical error, no verdict, cleared JSON and image path. */
    {
        cvf_inspection_request_v1 request = make_request("no.such.recipe", "b4-unknown-recipe");
        cvf_inspection_result_v1 result = make_result_full();
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        reset_result_buffers();
        status = cvf_inspect(context, &request, &result);
        check_status("b4_unknown_recipe", status, CVF_STATUS_RECIPE_NOT_FOUND);
        check_u32("b4_unknown_recipe", (uint32_t)result.verdict, CVF_VERDICT_NOT_EVALUATED,
                  "result.verdict");
        check_u32("b4_unknown_recipe", result.output_json_bytes_written, 0u,
                  "output_json_bytes_written");
        check_condition("b4_unknown_recipe", result.output_json[0] == '\0',
                        "output_json is cleared");
        check_u32("b4_unknown_recipe", result.image_path_bytes_written, 0u,
                  "image_path_bytes_written");
        check_condition("b4_unknown_recipe", result.image_path[0] == '\0', "image_path is cleared");
    }

    check_status("b4_shutdown", cvf_shutdown(context), CVF_STATUS_OK);
}

/*
 * The file-backed camera is used to prove that an undersized-buffer rejection
 * happens before acquisition: the frames directory holds a 2x2 frame first and a
 * 5x3 frame second, so if the rejected calls had consumed a frame the next
 * successful inspect would report total_pixels 15 instead of 4.
 */
static void case_b4_undersize_before_capture(void)
{
    char config_root[1024];
    char output_root[1024];
    char frames_dir[1024];
    cvf_context* context = NULL;

    make_case_dir(frames_dir, sizeof frames_dir, "b4_frames");
    (void)write_ppm_p6(frames_dir, "frame0.ppm", 2, 2, 0u);
    (void)write_ppm_p6(frames_dir, "frame1.ppm", 5, 3, 255u);

    make_case_dir(config_root, sizeof config_root, "b4_file_config");
    make_case_dir(output_root, sizeof output_root, "b4_file_output");
    (void)write_file_config(config_root, frames_dir);
    (void)add_recipe(config_root, "example.json", "example", "example.threshold", kPassParameters);

    if (!initialize_ok("b4_file_initialize", config_root, output_root, &context)) {
        return;
    }

    {
        cvf_inspection_request_v1 request = make_request("example", "b4-pre-capture-json");
        cvf_inspection_result_v1 result = make_result_caps(1024u, CVF_IMAGE_PATH_REQUIRED_CAPACITY,
                                                           CVF_ERROR_MESSAGE_REQUIRED_CAPACITY);

        check_status("b4_pre_capture_json_undersized", cvf_inspect(context, &request, &result),
                     CVF_STATUS_BUFFER_TOO_SMALL);
    }
    {
        cvf_inspection_request_v1 request = make_request("example", "b4-pre-capture-path");
        cvf_inspection_result_v1 result = make_result_caps(CVF_RESULT_JSON_REQUIRED_CAPACITY,
                                                           4095u,
                                                           CVF_ERROR_MESSAGE_REQUIRED_CAPACITY);

        check_status("b4_pre_capture_path_undersized", cvf_inspect(context, &request, &result),
                     CVF_STATUS_BUFFER_TOO_SMALL);
    }
    {
        cvf_inspection_request_v1 request = make_request("example", "b4-first-real-capture");
        cvf_inspection_result_v1 result;
        cvf_status_t status = CVF_STATUS_INTERNAL_ERROR;

        reset_result_buffers();
        result = make_result_full();
        status = cvf_inspect(context, &request, &result);
        check_status("b4_first_real_capture", status, CVF_STATUS_OK);
        check_u32("b4_first_real_capture", (uint32_t)result.verdict, CVF_VERDICT_PASS, "verdict");
        check_condition("b4_first_real_capture",
                        json_number_after(result.output_json, "total_pixels") == 4,
                        "the first real capture is still frame0 (2x2 -> total_pixels 4)");
    }

    check_status("b4_file_shutdown", cvf_shutdown(context), CVF_STATUS_OK);
}

/* ------------------------------------------------------------------------- */
/* B5: atomic recipe reload                                                   */
/* ------------------------------------------------------------------------- */

static cvf_status_t reload_with_error(cvf_context* context, const char* test_name,
                                      cvf_status_t expected)
{
    char message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
    cvf_error_info_v1 error = make_error(message, (uint32_t)sizeof message);
    cvf_status_t status = cvf_reload_recipes(context, &error);

    check_status(test_name, status, expected);
    if (expected == CVF_STATUS_OK) {
        check_u32(test_name, error.error_code, 0u, "error_code after a successful reload");
    } else {
        check_error_reported(test_name, &error, message);
    }
    return status;
}

static void case_b5_reload(void)
{
    char config_root[1024];
    char output_root[1024];
    cvf_context* context = NULL;

    make_case_dir(config_root, sizeof config_root, "b5_config");
    make_case_dir(output_root, sizeof output_root, "b5_output");
    (void)write_synthetic_config(config_root);
    (void)add_recipe(config_root, "example.json", "example", "example.threshold", kPassParameters);

    if (!initialize_ok("b5_initialize", config_root, output_root, &context)) {
        return;
    }

    /* A recipe that is not in the snapshot is not found before the reload. */
    {
        cvf_inspection_request_v1 request = make_request("added", "b5-before-reload");
        cvf_inspection_result_v1 result = make_result_full();
        cvf_status_t status = cvf_inspect(context, &request, &result);

        check_status("b5_added_recipe_not_found_before_reload", status, CVF_STATUS_RECIPE_NOT_FOUND);
    }

    /* Valid candidate: the new recipe becomes inspectable, the old one stays. */
    (void)add_recipe(config_root, "added.json", "added", "example.threshold", kPassParameters);
    if (reload_with_error(context, "b5_reload_adopts_new_recipe", CVF_STATUS_OK) == CVF_STATUS_OK) {
        cvf_inspection_request_v1 request = make_request("added", "b5-after-reload");
        cvf_inspection_result_v1 result = make_result_full();
        cvf_status_t status = cvf_inspect(context, &request, &result);

        check_status("b5_added_recipe_inspectable_after_reload", status, CVF_STATUS_OK);
        check_u32("b5_added_recipe_inspectable_after_reload", (uint32_t)result.verdict,
                  CVF_VERDICT_PASS, "verdict");
    }

    /* Re-loading the same content is a valid no-op. */
    (void)reload_with_error(context, "b5_noop_reload", CVF_STATUS_OK);
    {
        cvf_inspection_request_v1 request = make_request("example", "b5-after-noop");
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b5_original_still_inspectable", cvf_inspect(context, &request, &result),
                     CVF_STATUS_OK);
    }

    /* Invalid candidate with a bad parameter: rejected, old snapshot preserved. */
    (void)add_recipe(config_root, "broken.json", "broken", "example.threshold", "{\"threshold\": 999}");
    (void)reload_with_error(context, "b5_bad_parameter_rejected", CVF_STATUS_CONFIG_ERROR);
    {
        cvf_inspection_request_v1 request = make_request("example", "b5-after-bad-parameter");
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b5_old_snapshot_after_bad_parameter",
                     cvf_inspect(context, &request, &result), CVF_STATUS_OK);
        check_u32("b5_old_snapshot_after_bad_parameter", (uint32_t)result.verdict, CVF_VERDICT_PASS,
                  "verdict from the preserved snapshot");
    }
    (void)remove_recipe(config_root, "broken.json");

    /* Invalid candidate with an unknown algorithm: rejected, old snapshot kept. */
    (void)add_recipe(config_root, "unknown.json", "unknown.recipe", "no.such.algorithm",
                     kPassParameters);
    (void)reload_with_error(context, "b5_unknown_algorithm_rejected", CVF_STATUS_CONFIG_ERROR);
    {
        cvf_inspection_request_v1 request = make_request("example", "b5-after-unknown-algorithm");
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b5_old_snapshot_after_unknown_algorithm",
                     cvf_inspect(context, &request, &result), CVF_STATUS_OK);
    }
    (void)remove_recipe(config_root, "unknown.json");

    /* Invalid candidate with duplicate recipe ids: rejected, old snapshot kept. */
    (void)add_recipe(config_root, "dup_a.json", "duplicate", "example.threshold", kPassParameters);
    (void)add_recipe(config_root, "dup_b.json", "duplicate", "example.threshold", kPassParameters);
    (void)reload_with_error(context, "b5_duplicate_recipe_ids_rejected", CVF_STATUS_CONFIG_ERROR);
    {
        cvf_inspection_request_v1 request = make_request("example", "b5-after-duplicates");
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b5_old_snapshot_after_duplicates", cvf_inspect(context, &request, &result),
                     CVF_STATUS_OK);
    }
    (void)remove_recipe(config_root, "dup_a.json");
    (void)remove_recipe(config_root, "dup_b.json");

    /* After the directory is clean again, reload succeeds. */
    (void)reload_with_error(context, "b5_reload_succeeds_after_cleanup", CVF_STATUS_OK);

    check_status("b5_shutdown", cvf_shutdown(context), CVF_STATUS_OK);
}

/* ------------------------------------------------------------------------- */
/* B6: concurrent inspect calls on one context                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    cvf_context* context;
    const char* request_id;
    cvf_status_t status;
    int verdict_pass;
    int json_ok;
    char output_json[CVF_RESULT_JSON_REQUIRED_CAPACITY];
    char image_path[CVF_IMAGE_PATH_REQUIRED_CAPACITY];
    char error_message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];
} concurrent_case;

static concurrent_case g_concurrent[2];

static void concurrent_inspect(concurrent_case* item)
{
    cvf_inspection_request_v1 request;
    cvf_inspection_result_v1 result;

    memset(&request, 0, sizeof request);
    request.struct_size = (uint32_t)sizeof request;
    request.abi_version = CVF_ABI_VERSION_V1;
    request.recipe_id_utf8 = "example";
    request.recipe_id_utf8_bytes = (uint32_t)strlen("example");
    request.request_id_utf8 = item->request_id;
    request.request_id_utf8_bytes = (uint32_t)strlen(item->request_id);
    request.timeout_ms = 0u;

    memset(&result, 0, sizeof result);
    result.struct_size = (uint32_t)sizeof result;
    result.output_json = item->output_json;
    result.output_json_capacity = (uint32_t)sizeof item->output_json;
    result.image_path = item->image_path;
    result.image_path_capacity = (uint32_t)sizeof item->image_path;
    result.error_message = item->error_message;
    result.error_message_capacity = (uint32_t)sizeof item->error_message;

    item->status = cvf_inspect(item->context, &request, &result);
    item->verdict_pass = result.verdict == CVF_VERDICT_PASS;
    item->json_ok = json_contains_key(item->output_json, "white_pixels");
}

#ifdef _WIN32
static DWORD WINAPI concurrent_entry(LPVOID argument)
{
    concurrent_inspect((concurrent_case*)argument);
    return 0;
}
#else
static void* concurrent_entry(void* argument)
{
    concurrent_inspect((concurrent_case*)argument);
    return NULL;
}
#endif

static void case_b6_concurrency(void)
{
    char config_root[1024];
    char output_root[1024];
    cvf_context* context = NULL;
    int index = 0;

    make_case_dir(config_root, sizeof config_root, "b6_config");
    make_case_dir(output_root, sizeof output_root, "b6_output");
    (void)write_synthetic_config(config_root);
    (void)add_recipe(config_root, "example.json", "example", "example.threshold", kPassParameters);

    if (!initialize_ok("b6_initialize", config_root, output_root, &context)) {
        return;
    }

    memset(g_concurrent, 0, sizeof g_concurrent);
    g_concurrent[0].context = context;
    g_concurrent[0].request_id = "b6-thread-1";
    g_concurrent[1].context = context;
    g_concurrent[1].request_id = "b6-thread-2";

#ifdef _WIN32
    {
        HANDLE threads[2];
        threads[0] = CreateThread(NULL, 0, concurrent_entry, &g_concurrent[0], 0, NULL);
        threads[1] = CreateThread(NULL, 0, concurrent_entry, &g_concurrent[1], 0, NULL);
        check_condition("b6_threads_started", threads[0] != NULL && threads[1] != NULL,
                        "both inspect threads start");
        if (threads[0] != NULL) {
            (void)WaitForSingleObject(threads[0], 30000);
            (void)CloseHandle(threads[0]);
        }
        if (threads[1] != NULL) {
            (void)WaitForSingleObject(threads[1], 30000);
            (void)CloseHandle(threads[1]);
        }
    }
#else
    {
        pthread_t threads[2];
        int started[2];
        started[0] = pthread_create(&threads[0], NULL, concurrent_entry, &g_concurrent[0]) == 0;
        started[1] = pthread_create(&threads[1], NULL, concurrent_entry, &g_concurrent[1]) == 0;
        check_condition("b6_threads_started", started[0] && started[1],
                        "both inspect threads start");
        if (started[0]) {
            (void)pthread_join(threads[0], NULL);
        }
        if (started[1]) {
            (void)pthread_join(threads[1], NULL);
        }
    }
#endif

    for (index = 0; index < 2; ++index) {
        static const char* const kConcurrentNames[2] = {"b6_concurrent_inspect_1",
                                                        "b6_concurrent_inspect_2"};
        check_status(kConcurrentNames[index], g_concurrent[index].status, CVF_STATUS_OK);
        check_condition(kConcurrentNames[index], g_concurrent[index].verdict_pass,
                        "each concurrent inspect returns verdict PASS");
        check_condition(kConcurrentNames[index], g_concurrent[index].json_ok,
                        "each concurrent inspect returns uncorrupted measurement JSON");
    }

    check_status("b6_shutdown", cvf_shutdown(context), CVF_STATUS_OK);
}

/* ------------------------------------------------------------------------- */
/* B13: request identifier and input_json validation / passthrough            */
/* ------------------------------------------------------------------------- */

static void case_b13_request_validation(void)
{
    char config_root[1024];
    char output_root[1024];
    char long_id[130];
    char embedded_nul[4];
    char invalid_utf8_two[3];
    char invalid_utf8_one[2];
    char big_input[65537 + 1];
    const unsigned char invalid_utf8_two_lead = 0xC3u;
    const unsigned char invalid_utf8_one_lead = 0xFFu;
    cvf_context* context = NULL;

    make_case_dir(config_root, sizeof config_root, "b13_config");
    make_case_dir(output_root, sizeof output_root, "b13_output");
    (void)write_synthetic_config(config_root);
    (void)add_recipe(config_root, "pass.json", "example.pass", "example.threshold", kPassParameters);

    if (!initialize_ok("b13_initialize", config_root, output_root, &context)) {
        return;
    }

    memset(long_id, 'a', 129u);
    long_id[129] = '\0';
    embedded_nul[0] = 'a';
    embedded_nul[1] = '\0';
    embedded_nul[2] = 'b';
    embedded_nul[3] = '\0';
    /* MSVC C4310 rejects casting a constant above CHAR_MAX to char; route each
     * raw invalid-UTF-8 lead byte through a non-constant unsigned char. */
    invalid_utf8_two[0] = (char)invalid_utf8_two_lead;
    invalid_utf8_two[1] = 0x28;
    invalid_utf8_two[2] = '\0';
    invalid_utf8_one[0] = (char)invalid_utf8_one_lead;
    invalid_utf8_one[1] = '\0';

    /* Identifier length boundary: 129 bytes is invalid, 128 bytes is legal. */
    {
        cvf_inspection_request_v1 request = make_request_span(long_id, 129u, "b13-long-recipe",
                                                              (uint32_t)strlen("b13-long-recipe"),
                                                              0u, NULL, 0u);
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_recipe_id_too_long", cvf_inspect(context, &request, &result),
                     CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_inspection_request_v1 request = make_request_span("example.pass",
                                                              (uint32_t)strlen("example.pass"),
                                                              long_id, 129u, 0u, NULL, 0u);
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_request_id_too_long", cvf_inspect(context, &request, &result),
                     CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_inspection_request_v1 request = make_request_span(long_id, 128u, "b13-128-recipe",
                                                              text_bytes("b13-128-recipe"), 0u,
                                                              NULL, 0u);
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_recipe_id_128_bytes_accepted_as_identifier", cvf_inspect(context, &request, &result),
                     CVF_STATUS_RECIPE_NOT_FOUND);
    }

    /* Embedded NUL inside the declared span is invalid. */
    {
        cvf_inspection_request_v1 request = make_request_span(embedded_nul, 3u, "b13-embedded-nul",
                                                              text_bytes("b13-embedded-nul"), 0u,
                                                              NULL, 0u);
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_recipe_id_embedded_nul", cvf_inspect(context, &request, &result),
                     CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_inspection_request_v1 request = make_request_span("example.pass", 12u, embedded_nul, 3u,
                                                              0u, NULL, 0u);
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_request_id_embedded_nul", cvf_inspect(context, &request, &result),
                     CVF_STATUS_INVALID_ARGUMENT);
    }

    /* Invalid UTF-8 is invalid. */
    {
        cvf_inspection_request_v1 request = make_request_span("example.pass", 12u, invalid_utf8_two,
                                                              2u, 0u, NULL, 0u);
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_request_id_invalid_utf8", cvf_inspect(context, &request, &result),
                     CVF_STATUS_INVALID_ARGUMENT);
    }
    {
        cvf_inspection_request_v1 request = make_request_span(invalid_utf8_one, 1u, "b13-bad-utf8",
                                                              text_bytes("b13-bad-utf8"), 0u, NULL,
                                                              0u);
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_recipe_id_invalid_utf8", cvf_inspect(context, &request, &result),
                     CVF_STATUS_INVALID_ARGUMENT);
    }

    /* input_json must be a JSON object within the 65536 byte bound. */
    {
        static const char* kNotObjects[] = {"[1, 2]", "42", "\"text\""};
        size_t index = 0;
        for (index = 0u; index < sizeof kNotObjects / sizeof kNotObjects[0]; ++index) {
            cvf_inspection_request_v1 request = make_request_span(
                "example.pass", 12u, "b13-input-object", 16u, 0u, kNotObjects[index],
                (uint32_t)strlen(kNotObjects[index]));
            cvf_inspection_result_v1 result = make_result_full();

            check_status("b13_input_json_not_object", cvf_inspect(context, &request, &result),
                         CVF_STATUS_INVALID_ARGUMENT);
        }
    }
    {
        const char* padding = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        size_t offset = 0u;
        size_t index = 0u;
        big_input[0] = '{';
        big_input[1] = '"';
        big_input[2] = 'x';
        big_input[3] = '"';
        big_input[4] = ':';
        big_input[5] = '"';
        offset = 6u;
        for (index = 0u; index < 65529u; ++index) {
            big_input[offset + index] = padding[index % 64u];
        }
        big_input[65535] = '"';
        big_input[65536] = '}';
        big_input[65537] = '\0';

        {
            cvf_inspection_request_v1 request = make_request_span("example.pass", 12u,
                                                                  "b13-input-oversize",
                                                                  text_bytes("b13-input-oversize"),
                                                                  0u, big_input, 65537u);
            cvf_inspection_result_v1 result = make_result_full();

            check_status("b13_input_json_over_65536_bytes", cvf_inspect(context, &request, &result),
                         CVF_STATUS_INVALID_ARGUMENT);
        }
    }

    check_status("b13_shutdown", cvf_shutdown(context), CVF_STATUS_OK);
}

/*
 * Valid optional input_json must reach the algorithm: on an all-white frame the
 * example threshold recipe passes (threshold 128, min_pass_ratio 0.5), and
 * {"invert": true} flips the verdict to FAIL. Two identical white frames are
 * provided so both inspects capture a uniform frame.
 */
static void case_b13_input_json_passthrough(void)
{
    char config_root[1024];
    char output_root[1024];
    char frames_dir[1024];
    cvf_context* context = NULL;

    make_case_dir(frames_dir, sizeof frames_dir, "b13_frames");
    (void)write_ppm_p6(frames_dir, "frame0.ppm", 2, 2, 255u);
    (void)write_ppm_p6(frames_dir, "frame1.ppm", 2, 2, 255u);

    make_case_dir(config_root, sizeof config_root, "b13_file_config");
    make_case_dir(output_root, sizeof output_root, "b13_file_output");
    (void)write_file_config(config_root, frames_dir);
    (void)add_recipe(config_root, "flip.json", "example.flip", "example.threshold",
                     kFlipParameters);

    if (!initialize_ok("b13_file_initialize", config_root, output_root, &context)) {
        return;
    }

    {
        cvf_inspection_request_v1 request = make_request("example.flip", "b13-white-default");
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_uniform_frame_without_input_passes",
                     cvf_inspect(context, &request, &result), CVF_STATUS_OK);
        check_u32("b13_uniform_frame_without_input_passes", (uint32_t)result.verdict,
                  CVF_VERDICT_PASS, "verdict without input_json");
    }
    {
        static const char kInvertObject[] = "{\"invert\": true}";
        cvf_inspection_request_v1 request = make_request_span(
            "example.flip", 12u, "b13-white-inverted", 18u, 0u, kInvertObject,
            (uint32_t)strlen(kInvertObject));
        cvf_inspection_result_v1 result = make_result_full();

        check_status("b13_invert_flips_verdict", cvf_inspect(context, &request, &result),
                     CVF_STATUS_OK);
        check_u32("b13_invert_flips_verdict", (uint32_t)result.verdict, CVF_VERDICT_FAIL,
                  "verdict with invert input_json");
    }

    check_status("b13_file_shutdown", cvf_shutdown(context), CVF_STATUS_OK);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    char tmp_root[1024];
    char fixtures_root[1024];
    char pid_text[32];
    char tmp_tagged[2048];

    make_absolute_path(CVF_TEST_TMP_DIR, tmp_root, sizeof tmp_root);
    make_absolute_path(CVF_TEST_FIXTURES_DIR, fixtures_root, sizeof fixtures_root);
    concat_text(g_fixture_config_root, sizeof g_fixture_config_root, fixtures_root, "/valid_config");

    snprintf(pid_text, sizeof pid_text, "%ld", (long)
#ifdef _WIN32
                                                   _getpid()
#else
                                                   getpid()
#endif
    );
    concat_text(tmp_tagged, sizeof tmp_tagged, tmp_root, "/cvf006_");
    concat_text(g_base, sizeof g_base, tmp_tagged, pid_text);
    if (!make_dirs(g_base)) {
        printf("FAIL [setup] cannot create scratch directory %s\n", g_base);
        return 2;
    }

    printf("cvf006_lifecycle scratch: %s\n", g_base);

    case_b1_lifecycle();
    case_b2_initialize_failures();
    case_b3_inspect_happy_path();
    case_b3_verdict_ratio_boundary();
    case_b4_validation_and_errors();
    case_b4_undersize_before_capture();
    case_b5_reload();
    case_b6_concurrency();
    case_b13_request_validation();
    case_b13_input_json_passthrough();

    printf("cvf006_lifecycle: checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

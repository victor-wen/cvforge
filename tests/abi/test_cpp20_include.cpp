/*
 * CVF-001 independent black-box test - brief B4 (C++20 inclusion) and contract
 * field/signature exactness.
 *
 * Authored by test-engineer from .ai/test-briefs/CVF-001.yaml and the approved
 * .ai/project-contract.yaml only; no production implementation was consulted.
 *
 * RED / verify command (header must exist for this to compile):
 *   g++ -std=c++20 -Wall -Wextra -Werror -I include -fsyntax-only tests/abi/test_cpp20_include.cpp
 *
 * When linked against the cvforwin runtime this file also runs as a self-checking
 * executable: it calls cvf_get_abi_version() through C linkage and expects 1.
 */
#include <cstdint>
#include <cstdio>
#include <type_traits>

#include <cvforwin/cvf_api.h>

namespace cvf_test {

template <typename T, typename = void>
struct is_complete : std::false_type {};

template <typename T>
struct is_complete<T, std::void_t<decltype(sizeof(T))>> : std::true_type {};

}  // namespace cvf_test

/* The context must stay opaque: callers only ever hold cvf_context*. */
static_assert(!cvf_test::is_complete<cvf_context>::value,
              "cvf_context must remain an incomplete/opaque type");

/* Scalar ABI types. */
static_assert(std::is_same_v<cvf_status_t, std::uint32_t>, "cvf_status_t must be uint32_t");
static_assert(std::is_same_v<cvf_verdict_t, std::uint32_t>, "cvf_verdict_t must be uint32_t");

/* ---- B3: exact numeric constants ---- */

static_assert(CVF_ABI_VERSION_V1 == 1, "CVF_ABI_VERSION_V1 must be 1");
static_assert(CVF_RECIPE_ID_MAX_UTF8_BYTES == 128u, "CVF_RECIPE_ID_MAX_UTF8_BYTES must be 128");
static_assert(CVF_REQUEST_ID_MAX_UTF8_BYTES == 128u, "CVF_REQUEST_ID_MAX_UTF8_BYTES must be 128");
static_assert(CVF_INPUT_JSON_MAX_UTF8_BYTES == 65536u, "CVF_INPUT_JSON_MAX_UTF8_BYTES must be 65536");
static_assert(CVF_RESULT_JSON_REQUIRED_CAPACITY == 65536u, "CVF_RESULT_JSON_REQUIRED_CAPACITY must be 65536");
static_assert(CVF_ERROR_MESSAGE_REQUIRED_CAPACITY == 1024u, "CVF_ERROR_MESSAGE_REQUIRED_CAPACITY must be 1024");
static_assert(CVF_IMAGE_PATH_REQUIRED_CAPACITY == 4096u, "CVF_IMAGE_PATH_REQUIRED_CAPACITY must be 4096");

static_assert(CVF_STATUS_OK == 0u, "CVF_STATUS_OK must be 0");
static_assert(CVF_STATUS_INVALID_ARGUMENT == 1u, "CVF_STATUS_INVALID_ARGUMENT must be 1");
static_assert(CVF_STATUS_ABI_MISMATCH == 2u, "CVF_STATUS_ABI_MISMATCH must be 2");
static_assert(CVF_STATUS_CONTEXT_LIMIT == 3u, "CVF_STATUS_CONTEXT_LIMIT must be 3");
static_assert(CVF_STATUS_INVALID_CONTEXT == 4u, "CVF_STATUS_INVALID_CONTEXT must be 4");
static_assert(CVF_STATUS_CONFIG_ERROR == 5u, "CVF_STATUS_CONFIG_ERROR must be 5");
static_assert(CVF_STATUS_RECIPE_NOT_FOUND == 6u, "CVF_STATUS_RECIPE_NOT_FOUND must be 6");
static_assert(CVF_STATUS_CAMERA_NOT_FOUND == 7u, "CVF_STATUS_CAMERA_NOT_FOUND must be 7");
static_assert(CVF_STATUS_CAMERA_IO == 8u, "CVF_STATUS_CAMERA_IO must be 8");
static_assert(CVF_STATUS_TIMEOUT == 9u, "CVF_STATUS_TIMEOUT must be 9");
static_assert(CVF_STATUS_BUFFER_TOO_SMALL == 10u, "CVF_STATUS_BUFFER_TOO_SMALL must be 10");
static_assert(CVF_STATUS_ALGORITHM_ERROR == 11u, "CVF_STATUS_ALGORITHM_ERROR must be 11");
static_assert(CVF_STATUS_REQUIRED_ARTIFACT_ERROR == 12u, "CVF_STATUS_REQUIRED_ARTIFACT_ERROR must be 12");
static_assert(CVF_STATUS_INTERNAL_ERROR == 13u, "CVF_STATUS_INTERNAL_ERROR must be 13");

static_assert(CVF_VERDICT_NOT_EVALUATED == 0u, "CVF_VERDICT_NOT_EVALUATED must be 0");
static_assert(CVF_VERDICT_PASS == 1u, "CVF_VERDICT_PASS must be 1");
static_assert(CVF_VERDICT_FAIL == 2u, "CVF_VERDICT_FAIL must be 2");

static_assert(CVF_INIT_FLAG_FILE_LOGGING == 1u, "CVF_INIT_FLAG_FILE_LOGGING must be 1u << 0");
static_assert(CVF_INIT_FLAG_CALLBACK_LOGGING == 2u, "CVF_INIT_FLAG_CALLBACK_LOGGING must be 1u << 1");

/* ---- Exact field types ---- */

static_assert(std::is_same_v<decltype(cvf_init_options_v1::struct_size), std::uint32_t>, "cvf_init_options_v1::struct_size");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::abi_version), std::uint32_t>, "cvf_init_options_v1::abi_version");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::config_root_utf8), const char*>, "cvf_init_options_v1::config_root_utf8");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::config_root_utf8_bytes), std::uint32_t>, "cvf_init_options_v1::config_root_utf8_bytes");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::output_root_utf8), const char*>, "cvf_init_options_v1::output_root_utf8");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::output_root_utf8_bytes), std::uint32_t>, "cvf_init_options_v1::output_root_utf8_bytes");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::flags), std::uint32_t>, "cvf_init_options_v1::flags");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::log_callback), cvf_log_callback>, "cvf_init_options_v1::log_callback");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::user_data), void*>, "cvf_init_options_v1::user_data");
static_assert(std::is_same_v<decltype(cvf_init_options_v1::reserved), std::uint32_t[8]>, "cvf_init_options_v1::reserved");

static_assert(std::is_same_v<decltype(cvf_error_info_v1::struct_size), std::uint32_t>, "cvf_error_info_v1::struct_size");
static_assert(std::is_same_v<decltype(cvf_error_info_v1::error_code), std::uint32_t>, "cvf_error_info_v1::error_code");
static_assert(std::is_same_v<decltype(cvf_error_info_v1::message_utf8), char*>, "cvf_error_info_v1::message_utf8");
static_assert(std::is_same_v<decltype(cvf_error_info_v1::message_capacity), std::uint32_t>, "cvf_error_info_v1::message_capacity");
static_assert(std::is_same_v<decltype(cvf_error_info_v1::message_bytes_written), std::uint32_t>, "cvf_error_info_v1::message_bytes_written");
static_assert(std::is_same_v<decltype(cvf_error_info_v1::message_bytes_required), std::uint32_t>, "cvf_error_info_v1::message_bytes_required");
static_assert(std::is_same_v<decltype(cvf_error_info_v1::reserved), std::uint32_t[4]>, "cvf_error_info_v1::reserved");

static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::struct_size), std::uint32_t>, "cvf_inspection_request_v1::struct_size");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::abi_version), std::uint32_t>, "cvf_inspection_request_v1::abi_version");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::recipe_id_utf8), const char*>, "cvf_inspection_request_v1::recipe_id_utf8");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::recipe_id_utf8_bytes), std::uint32_t>, "cvf_inspection_request_v1::recipe_id_utf8_bytes");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::request_id_utf8), const char*>, "cvf_inspection_request_v1::request_id_utf8");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::request_id_utf8_bytes), std::uint32_t>, "cvf_inspection_request_v1::request_id_utf8_bytes");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::timeout_ms), std::uint32_t>, "cvf_inspection_request_v1::timeout_ms");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::input_json_utf8), const char*>, "cvf_inspection_request_v1::input_json_utf8");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::input_json_utf8_bytes), std::uint32_t>, "cvf_inspection_request_v1::input_json_utf8_bytes");
static_assert(std::is_same_v<decltype(cvf_inspection_request_v1::reserved), std::uint32_t[8]>, "cvf_inspection_request_v1::reserved");

static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::struct_size), std::uint32_t>, "cvf_inspection_result_v1::struct_size");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::status), cvf_status_t>, "cvf_inspection_result_v1::status");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::verdict), cvf_verdict_t>, "cvf_inspection_result_v1::verdict");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::error_code), std::uint32_t>, "cvf_inspection_result_v1::error_code");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::warning_flags), std::uint32_t>, "cvf_inspection_result_v1::warning_flags");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::elapsed_ms), std::uint32_t>, "cvf_inspection_result_v1::elapsed_ms");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::output_json), char*>, "cvf_inspection_result_v1::output_json");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::output_json_capacity), std::uint32_t>, "cvf_inspection_result_v1::output_json_capacity");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::output_json_bytes_written), std::uint32_t>, "cvf_inspection_result_v1::output_json_bytes_written");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::output_json_bytes_required), std::uint32_t>, "cvf_inspection_result_v1::output_json_bytes_required");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::image_path), char*>, "cvf_inspection_result_v1::image_path");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::image_path_capacity), std::uint32_t>, "cvf_inspection_result_v1::image_path_capacity");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::image_path_bytes_written), std::uint32_t>, "cvf_inspection_result_v1::image_path_bytes_written");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::image_path_bytes_required), std::uint32_t>, "cvf_inspection_result_v1::image_path_bytes_required");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::error_message), char*>, "cvf_inspection_result_v1::error_message");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::error_message_capacity), std::uint32_t>, "cvf_inspection_result_v1::error_message_capacity");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::error_message_bytes_written), std::uint32_t>, "cvf_inspection_result_v1::error_message_bytes_written");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::error_message_bytes_required), std::uint32_t>, "cvf_inspection_result_v1::error_message_bytes_required");
static_assert(std::is_same_v<decltype(cvf_inspection_result_v1::reserved), std::uint32_t[8]>, "cvf_inspection_result_v1::reserved");

/* ---- B4: exact function signatures ---- */

static_assert(std::is_same_v<decltype(&cvf_get_abi_version), std::uint32_t (*)()>, "cvf_get_abi_version signature");
static_assert(std::is_same_v<decltype(&cvf_initialize), cvf_status_t (*)(const cvf_init_options_v1*, cvf_context**, cvf_error_info_v1*)>, "cvf_initialize signature");
static_assert(std::is_same_v<decltype(&cvf_reload_recipes), cvf_status_t (*)(cvf_context*, cvf_error_info_v1*)>, "cvf_reload_recipes signature");
static_assert(std::is_same_v<decltype(&cvf_inspect), cvf_status_t (*)(cvf_context*, const cvf_inspection_request_v1*, cvf_inspection_result_v1*)>, "cvf_inspect signature");
static_assert(std::is_same_v<decltype(&cvf_shutdown), cvf_status_t (*)(cvf_context*)>, "cvf_shutdown signature");

namespace {

void CVF_CALL cpp_log_callback(uint32_t level, const char* message_utf8, void* user_data)
{
    (void)level;
    (void)message_utf8;
    (void)user_data;
}

}  // namespace

int main()
{
    cvf_log_callback callback = &cpp_log_callback;
    const uint32_t version = cvf_get_abi_version();

    if (callback == nullptr) {
        std::fprintf(stderr, "FAIL [cpp20_callback_type] cvf_log_callback is unusable\n");
        return 1;
    }
    if (version != CVF_ABI_VERSION_V1) {
        std::fprintf(stderr, "FAIL [cpp20_abi_version] expected 1, observed %u\n",
                     static_cast<unsigned>(version));
        return 1;
    }
    std::printf("ok   [cpp20_abi_version] %u\n", static_cast<unsigned>(version));
    return 0;
}

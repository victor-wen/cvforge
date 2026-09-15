/*
 * Developer regression tests for failed-inspection result hygiene.
 *
 * A non-OK cvf_inspect call must publish its status into a valid v1 result
 * structure, force verdict NOT_EVALUATED, clear output_json and image_path
 * (zero bytes written and required, NUL-terminated when capacity >= 1), and
 * write the diagnostic into error_message. A malformed result structure must
 * never be modified through any failure path.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include <cvforwin/cvf_api.h>

namespace {

constexpr char kRecipeId[] = "example";
constexpr char kRequestId[] = "request-1";

char g_output_json[CVF_RESULT_JSON_REQUIRED_CAPACITY];
char g_image_path[CVF_IMAGE_PATH_REQUIRED_CAPACITY];
char g_error_message[CVF_ERROR_MESSAGE_REQUIRED_CAPACITY];

cvf_inspection_request_v1 make_request()
{
    cvf_inspection_request_v1 request{};
    request.struct_size = static_cast<std::uint32_t>(sizeof request);
    request.abi_version = CVF_ABI_VERSION_V1;
    request.recipe_id_utf8 = kRecipeId;
    request.recipe_id_utf8_bytes = static_cast<std::uint32_t>(sizeof kRecipeId) - 1u;
    request.request_id_utf8 = kRequestId;
    request.request_id_utf8_bytes = static_cast<std::uint32_t>(sizeof kRequestId) - 1u;
    return request;
}

cvf_inspection_result_v1 make_result()
{
    cvf_inspection_result_v1 result{};
    result.struct_size = static_cast<std::uint32_t>(sizeof result);
    result.output_json = g_output_json;
    result.output_json_capacity = static_cast<std::uint32_t>(sizeof g_output_json);
    result.output_json_bytes_written = 7u;
    result.output_json_bytes_required = 9u;
    result.image_path = g_image_path;
    result.image_path_capacity = static_cast<std::uint32_t>(sizeof g_image_path);
    result.image_path_bytes_written = 3u;
    result.image_path_bytes_required = 4u;
    result.error_message = g_error_message;
    result.error_message_capacity = static_cast<std::uint32_t>(sizeof g_error_message);
    return result;
}

}  // namespace

TEST_CASE("a failed inspect publishes NOT_EVALUATED and clears output paths", "[unit][c_api][hygiene]")
{
    cvf_inspection_request_v1 request = make_request();
    cvf_inspection_result_v1 result = make_result();
    g_output_json[0] = 'x';
    g_image_path[0] = 'y';

    const cvf_status_t status = cvf_inspect(nullptr, &request, &result);

    CHECK(status == CVF_STATUS_INVALID_CONTEXT);
    CHECK(result.status == CVF_STATUS_INVALID_CONTEXT);
    CHECK(result.verdict == CVF_VERDICT_NOT_EVALUATED);
    CHECK(result.error_code != 0u);
    CHECK(result.output_json_bytes_written == 0u);
    CHECK(result.output_json_bytes_required == 0u);
    CHECK(g_output_json[0] == '\0');
    CHECK(result.image_path_bytes_written == 0u);
    CHECK(result.image_path_bytes_required == 0u);
    CHECK(g_image_path[0] == '\0');
    CHECK(result.error_message_bytes_written > 0u);
    CHECK(g_error_message[result.error_message_bytes_written] == '\0');
}

TEST_CASE("a malformed inspect result structure is never modified", "[unit][c_api][hygiene]")
{
    SECTION("wrong struct size")
    {
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        result.struct_size = 0u;
        result.verdict = CVF_VERDICT_PASS;
        result.output_json_bytes_written = 0xDEADBEEFu;
        result.image_path_bytes_written = 0xBEEFu;
        g_output_json[0] = 'x';

        const cvf_status_t status = cvf_inspect(nullptr, &request, &result);

        CHECK(status != CVF_STATUS_OK);
        CHECK(result.struct_size == 0u);
        CHECK(result.verdict == CVF_VERDICT_PASS);
        CHECK(result.output_json_bytes_written == 0xDEADBEEFu);
        CHECK(result.image_path_bytes_written == 0xBEEFu);
        CHECK(g_output_json[0] == 'x');
    }

    SECTION("nonzero reserved words")
    {
        cvf_inspection_request_v1 request = make_request();
        cvf_inspection_result_v1 result = make_result();
        result.reserved[3] = 1u;
        result.verdict = CVF_VERDICT_FAIL;
        result.output_json_bytes_written = 0xDEADBEEFu;
        g_output_json[0] = 'x';

        const cvf_status_t status = cvf_inspect(nullptr, &request, &result);

        CHECK(status != CVF_STATUS_OK);
        CHECK(result.reserved[3] == 1u);
        CHECK(result.verdict == CVF_VERDICT_FAIL);
        CHECK(result.output_json_bytes_written == 0xDEADBEEFu);
        CHECK(g_output_json[0] == 'x');
    }
}

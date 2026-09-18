// CVF-105 independent black-box tests: end-to-end C ABI pointer-length/capacity
// pairs through a real single context and the deterministic synthetic backend.
//
// Brief B3/B4: input_json NULL+0 is absent; NULL+nonzero is INVALID_ARGUMENT
// before side effects; non-NULL+0 is absent; non-NULL+positive is parsed. The
// same four combinations hold for recipe_id/request_id/root text buffers, and
// output_json/image_path/error_message capacities reject NULL+nonzero and
// follow BUFFER_TOO_SMALL for non-NULL+zero. B7: the ABI copies the exact
// pre-serialized string, NUL-terminated with matching byte counts.
//
// Gated on CVFORWIN_BUILD_TEST_BACKENDS because the config backend "synthetic"
// exists only in test-enabled builds. Hardware-free and deterministic.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "cvf105_test.helpers.h"

#include "cvforwin/cvf_api.h"

namespace {

using cvf105::Json;
using cvf105::TempDir;

struct ErrorBuffer {
    std::array<char, CVF_ERROR_MESSAGE_REQUIRED_CAPACITY> storage{};
    cvf_error_info_v1 info{};

    ErrorBuffer()
    {
        info.struct_size = sizeof(cvf_error_info_v1);
        info.message_utf8 = storage.data();
        info.message_capacity = static_cast<std::uint32_t>(storage.size());
    }
};

struct ResultBuffer {
    std::array<char, CVF_RESULT_JSON_REQUIRED_CAPACITY> output{};
    std::array<char, CVF_IMAGE_PATH_REQUIRED_CAPACITY> image{};
    std::array<char, CVF_ERROR_MESSAGE_REQUIRED_CAPACITY> message{};
    cvf_inspection_result_v1 result{};

    ResultBuffer()
    {
        result.struct_size = sizeof(cvf_inspection_result_v1);
        result.output_json = output.data();
        result.output_json_capacity = static_cast<std::uint32_t>(output.size());
        result.image_path = image.data();
        result.image_path_capacity = static_cast<std::uint32_t>(image.size());
        result.error_message = message.data();
        result.error_message_capacity = static_cast<std::uint32_t>(message.size());
    }
};

struct RunningContext {
    cvf_context* context = nullptr;
    ErrorBuffer error{};

    RunningContext(const std::string& config_root, const std::string& output_root,
                   cvf_status_t expected = CVF_STATUS_OK)
    {
        cvf_init_options_v1 options{};
        options.struct_size = sizeof(cvf_init_options_v1);
        options.abi_version = CVF_ABI_VERSION_V1;
        options.config_root_utf8 = config_root.c_str();
        options.config_root_utf8_bytes = static_cast<std::uint32_t>(config_root.size());
        options.output_root_utf8 = output_root.c_str();
        options.output_root_utf8_bytes = static_cast<std::uint32_t>(output_root.size());

        const cvf_status_t status = cvf_initialize(&options, &context, &error.info);
        INFO("initialize status: " << status);
        REQUIRE(status == expected);
    }

    ~RunningContext()
    {
        if (context != nullptr) {
            cvf_shutdown(context);
        }
    }

    RunningContext(const RunningContext&) = delete;
    RunningContext& operator=(const RunningContext&) = delete;
};

struct RequestHolder {
    std::string recipe_id;
    std::string request_id;
    std::string input_json;
    cvf_inspection_request_v1 request{};

    RequestHolder(std::string recipe, std::string request_name, std::string input)
        : recipe_id(std::move(recipe)), request_id(std::move(request_name)), input_json(std::move(input))
    {
        request.struct_size = sizeof(cvf_inspection_request_v1);
        request.abi_version = CVF_ABI_VERSION_V1;
        request.recipe_id_utf8 = recipe_id.c_str();
        request.recipe_id_utf8_bytes = static_cast<std::uint32_t>(recipe_id.size());
        request.request_id_utf8 = request_id.c_str();
        request.request_id_utf8_bytes = static_cast<std::uint32_t>(request_id.size());
        request.timeout_ms = 0u;
        request.input_json_utf8 = input_json.empty() ? nullptr : input_json.c_str();
        request.input_json_utf8_bytes = static_cast<std::uint32_t>(input_json.size());
    }
};

void write_synthetic_root(const TempDir& root)
{
    cvf105::write_config_root(root.path(), cvf105::synthetic_config_json());
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* B3: input_json pointer-length combinations.                                */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B3: input_json NULL+0 and non-NULL+0 are absent and non-NULL+positive is parsed",
          "[cvf-105][B3][abi][pairs]")
{
    TempDir root("abi_input_json");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    SECTION("NULL pointer with zero length is absent")
    {
        RequestHolder holder("example", "b3-null-zero", "");
        holder.request.input_json_utf8 = nullptr;
        holder.request.input_json_utf8_bytes = 0u;
        ResultBuffer buffer;

        const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);

        CHECK(status == CVF_STATUS_OK);
        CHECK(buffer.result.status == CVF_STATUS_OK);
    }

    SECTION("non-NULL pointer with zero length is absent")
    {
        RequestHolder holder("example", "b3-nonnull-zero", "{}");
        holder.request.input_json_utf8_bytes = 0u;
        ResultBuffer buffer;

        const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);

        CHECK(status == CVF_STATUS_OK);
    }

    SECTION("non-NULL pointer with positive length is parsed")
    {
        RequestHolder holder("example", "b3-nonnull-positive", "{}");
        ResultBuffer buffer;

        const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);

        CHECK(status == CVF_STATUS_OK);
        CHECK(buffer.result.status == CVF_STATUS_OK);
    }
}

TEST_CASE("CVF-105 B3 negative: input_json NULL with a nonzero length is INVALID_ARGUMENT",
          "[cvf-105][B3][abi][pairs][negative]")
{
    TempDir root("abi_input_json_null_pos");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    RequestHolder holder("example", "b3-null-positive", "{}");
    holder.request.input_json_utf8 = nullptr;
    holder.request.input_json_utf8_bytes = 2u;
    ResultBuffer buffer;

    const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);

    CHECK(status == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(buffer.result.status == CVF_STATUS_INVALID_ARGUMENT);
    CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
}

TEST_CASE("CVF-105 B3 negative: invalid JSON and a non-object top level are rejected",
          "[cvf-105][B3][abi][pairs][negative]")
{
    TempDir root("abi_input_json_shape");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    SECTION("malformed JSON")
    {
        RequestHolder holder("example", "b3-malformed", "{\"a\":");
        ResultBuffer buffer;
        const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);
        CHECK(status == CVF_STATUS_INVALID_ARGUMENT);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }
    SECTION("non-object top level")
    {
        RequestHolder holder("example", "b3-array", "[1,2,3]");
        ResultBuffer buffer;
        const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);
        CHECK(status == CVF_STATUS_INVALID_ARGUMENT);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }
    SECTION("object with surrounding whitespace is accepted")
    {
        RequestHolder holder("example", "b3-whitespace", "  { \"invert\" : false }  ");
        ResultBuffer buffer;
        const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);
        CHECK(status == CVF_STATUS_OK);
    }
    SECTION("malformed JSON with surrounding whitespace is rejected")
    {
        RequestHolder holder("example", "b3-whitespace-malformed", "  { \"a\" : }  ");
        ResultBuffer buffer;
        const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);
        CHECK(status == CVF_STATUS_INVALID_ARGUMENT);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }
}

/* ------------------------------------------------------------------------- */
/* B4: recipe_id and request_id pointer-length combinations.                  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B4: recipe_id/request_id NULL+nonzero are INVALID_ARGUMENT before side effects",
          "[cvf-105][B4][abi][pairs][negative]")
{
    TempDir root("abi_ids_null_pos");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    SECTION("recipe_id NULL with nonzero length")
    {
        RequestHolder holder("example", "b4-recipe-null", "");
        holder.request.recipe_id_utf8 = nullptr;
        holder.request.recipe_id_utf8_bytes = 7u;
        ResultBuffer buffer;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
        CHECK(buffer.result.status == CVF_STATUS_INVALID_ARGUMENT);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }
    SECTION("recipe_id NULL with zero length")
    {
        RequestHolder holder("example", "b4-recipe-null-zero", "");
        holder.request.recipe_id_utf8 = nullptr;
        holder.request.recipe_id_utf8_bytes = 0u;
        ResultBuffer buffer;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("recipe_id non-NULL with zero length")
    {
        RequestHolder holder("example", "b4-recipe-zero", "");
        holder.request.recipe_id_utf8_bytes = 0u;
        ResultBuffer buffer;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("request_id NULL with nonzero length")
    {
        RequestHolder holder("example", "b4-request-null", "");
        holder.request.request_id_utf8 = nullptr;
        holder.request.request_id_utf8_bytes = 4u;
        ResultBuffer buffer;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("request_id NULL with zero length")
    {
        RequestHolder holder("example", "b4-request-null-zero", "");
        holder.request.request_id_utf8 = nullptr;
        holder.request.request_id_utf8_bytes = 0u;
        ResultBuffer buffer;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("request_id non-NULL with zero length")
    {
        RequestHolder holder("example", "b4-request-zero", "");
        holder.request.request_id_utf8_bytes = 0u;
        ResultBuffer buffer;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
    }
}

/* ------------------------------------------------------------------------- */
/* B4: output buffer capacities reject NULL+nonzero; zero capacity follows     */
/* BUFFER_TOO_SMALL.                                                          */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B4: result output/image/error capacities follow the pointer-capacity rules",
          "[cvf-105][B4][abi][pairs][negative]")
{
    TempDir root("abi_output_caps");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    SECTION("output_json NULL with nonzero capacity")
    {
        RequestHolder holder("example", "b4-out-null", "");
        ResultBuffer buffer;
        buffer.result.output_json = nullptr;
        buffer.result.output_json_capacity = CVF_RESULT_JSON_REQUIRED_CAPACITY;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }
    SECTION("image_path NULL with nonzero capacity")
    {
        RequestHolder holder("example", "b4-img-null", "");
        ResultBuffer buffer;
        buffer.result.image_path = nullptr;
        buffer.result.image_path_capacity = CVF_IMAGE_PATH_REQUIRED_CAPACITY;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
    }
    SECTION("error_message NULL with nonzero capacity")
    {
        RequestHolder holder("example", "b4-msg-null", "");
        ResultBuffer buffer;
        buffer.result.error_message = nullptr;
        buffer.result.error_message_capacity = CVF_ERROR_MESSAGE_REQUIRED_CAPACITY;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_INVALID_ARGUMENT);
    }
}

/* ------------------------------------------------------------------------- */
/* B7: the emitted JSON is the exact, NUL-terminated serialized object.       */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B7: the emitted output_json is valid, NUL-terminated, and matches its byte counts",
          "[cvf-105][B7][abi]")
{
    TempDir root("abi_payload_copy");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    RequestHolder holder("example", "b7-copy", "{}");
    ResultBuffer buffer;

    const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);

    REQUIRE(status == CVF_STATUS_OK);
    REQUIRE(buffer.result.status == CVF_STATUS_OK);

    const std::uint32_t written = buffer.result.output_json_bytes_written;
    REQUIRE(written < buffer.output.size());
    CHECK(buffer.output[written] == '\0');
    CHECK(std::strlen(buffer.output.data()) == written);
    CHECK(buffer.result.output_json_bytes_required == written + 1u);

    const Json emitted = Json::parse(buffer.output.data());
    CHECK(emitted.is_object());
    CHECK(emitted.dump() == std::string(buffer.output.data()));
}

TEST_CASE("CVF-105 B7: repeated identical inspections emit the identical serialized payload",
          "[cvf-105][B7][abi]")
{
    TempDir root("abi_payload_determinism");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    RequestHolder first_request("example", "b7-first", "{}");
    ResultBuffer first;
    REQUIRE(cvf_inspect(running.context, &first_request.request, &first.result) == CVF_STATUS_OK);

    RequestHolder second_request("example", "b7-second", "{}");
    ResultBuffer second;
    REQUIRE(cvf_inspect(running.context, &second_request.request, &second.result) == CVF_STATUS_OK);

    CHECK(std::string(first.output.data()) == std::string(second.output.data()));
    CHECK(first.result.output_json_bytes_written == second.result.output_json_bytes_written);
}

/* ------------------------------------------------------------------------- */
/* B3/B4: a closed or invalid context is INVALID_CONTEXT; a valid context      */
/* keeps working after rejected calls.                                        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B3: a rejected pointer-length pair leaves the context usable",
          "[cvf-105][B3][abi]")
{
    TempDir root("abi_context_survives");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    RequestHolder bad("example", "b3-bad", "{}");
    bad.request.input_json_utf8 = nullptr;
    bad.request.input_json_utf8_bytes = 3u;
    ResultBuffer bad_buffer;
    REQUIRE(cvf_inspect(running.context, &bad.request, &bad_buffer.result) ==
            CVF_STATUS_INVALID_ARGUMENT);

    RequestHolder good("example", "b3-good", "{}");
    ResultBuffer good_buffer;
    CHECK(cvf_inspect(running.context, &good.request, &good_buffer.result) == CVF_STATUS_OK);
    CHECK(good_buffer.result.verdict != CVF_VERDICT_NOT_EVALUATED);
}

/* ------------------------------------------------------------------------- */
/* B4 boundary: a non-null output buffer with zero capacity is a legal        */
/* pointer-capacity pair and follows the BUFFER_TOO_SMALL rule.               */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B4 boundary: non-null result buffers with zero capacity report "
          "BUFFER_TOO_SMALL",
          "[cvf-105][B4][abi][pairs][boundary][negative]")
{
    TempDir root("abi_output_zero_capacity");
    write_synthetic_root(root);
    RunningContext running(root.path().string(), (root.path() / "out").string());

    SECTION("output_json non-null with zero capacity")
    {
        RequestHolder holder("example", "b4-out-zero", "{}");
        ResultBuffer buffer;
        REQUIRE(buffer.result.output_json != nullptr);
        buffer.result.output_json_capacity = 0u;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_BUFFER_TOO_SMALL);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }
    SECTION("image_path non-null with zero capacity")
    {
        RequestHolder holder("example", "b4-img-zero", "{}");
        ResultBuffer buffer;
        REQUIRE(buffer.result.image_path != nullptr);
        buffer.result.image_path_capacity = 0u;
        CHECK(cvf_inspect(running.context, &holder.request, &buffer.result) ==
              CVF_STATUS_BUFFER_TOO_SMALL);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }
    SECTION("error_message non-null with zero capacity is a legal pair and writes nothing on "
            "success")
    {
        RequestHolder holder("example", "b4-msg-zero", "{}");
        ResultBuffer buffer;
        REQUIRE(buffer.result.error_message != nullptr);
        buffer.result.error_message_capacity = 0u;
        // The error-message channel is only populated on failure; a legal
        // non-null+zero pair therefore does not by itself fail a successful
        // inspection. (The required-capacity output_json and image_path buffers
        // above do enforce BUFFER_TOO_SMALL.)
        buffer.result.error_message_bytes_written = 0u;
        buffer.result.error_message_bytes_required = 0u;
        const cvf_status_t status = cvf_inspect(running.context, &holder.request, &buffer.result);
        CHECK(status == CVF_STATUS_OK);
        CHECK(buffer.result.status == CVF_STATUS_OK);
        CHECK(buffer.result.verdict != CVF_VERDICT_NOT_EVALUATED);
        CHECK(buffer.result.error_message_bytes_written == 0u);
    }
}

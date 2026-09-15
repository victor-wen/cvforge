/*
 * Developer unit tests for the internal ABI validation helpers.
 *
 * The helpers are ABI-independent: the expected v1 struct size is supplied by
 * the caller, exactly as src/c_api supplies the real sizeof values from the
 * public header. The expected sizes below are derived from the frozen public
 * header itself instead of being hard-coded, so a layout mistake cannot be
 * hidden by a matching literal. On the supported 64-bit targets the v1 sizes
 * are: init options 88, error info 48, request 88, result 128.
 */

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

#include <cvforwin/cvf_api.h>

#include "core/abi_validation.h"
#include "core/error.h"

using cvforwin::core::ErrorCode;
using cvforwin::core::Status;

namespace {

constexpr std::size_t kInitSize = sizeof(cvf_init_options_v1);
constexpr std::size_t kErrorSize = sizeof(cvf_error_info_v1);
constexpr std::size_t kRequestSize = sizeof(cvf_inspection_request_v1);
constexpr std::size_t kResultSize = sizeof(cvf_inspection_result_v1);

cvforwin::core::InitOptionsView valid_options()
{
    cvforwin::core::InitOptionsView view;
    view.struct_size = static_cast<std::uint32_t>(kInitSize);
    view.abi_version = cvforwin::core::kAbiVersionV1;
    view.config_root = "/opt/cvforwin/config";
    view.output_root = "/opt/cvforwin/output";
    view.flags = cvforwin::core::kInitFlagFileLogging;
    return view;
}

cvforwin::core::ErrorInfoView valid_error()
{
    cvforwin::core::ErrorInfoView view;
    view.struct_size = static_cast<std::uint32_t>(kErrorSize);
    view.message_present = true;
    view.message_capacity = 1024u;
    return view;
}

cvforwin::core::InspectionRequestView valid_request()
{
    cvforwin::core::InspectionRequestView view;
    view.struct_size = static_cast<std::uint32_t>(kRequestSize);
    view.abi_version = cvforwin::core::kAbiVersionV1;
    view.recipe_id = "example";
    view.request_id = "request-1";
    return view;
}

cvforwin::core::InspectionResultView valid_result()
{
    cvforwin::core::InspectionResultView view;
    view.struct_size = static_cast<std::uint32_t>(kResultSize);
    view.output_json_present = true;
    view.output_json_capacity = cvforwin::core::kRequiredResultJsonCapacity;
    view.image_path_present = true;
    view.image_path_capacity = cvforwin::core::kRequiredImagePathCapacity;
    view.error_message_present = true;
    view.error_message_capacity = cvforwin::core::kRequiredErrorMessageCapacity;
    return view;
}

}  // namespace

TEST_CASE("valid v1 views are accepted", "[core][validation]")
{
    CHECK_FALSE(cvforwin::core::validate_error_info(valid_error(), kErrorSize).has_value());
    CHECK_FALSE(cvforwin::core::validate_init_options(valid_options(), kInitSize).has_value());
    CHECK_FALSE(cvforwin::core::validate_inspection_request(valid_request(), kRequestSize).has_value());
    CHECK_FALSE(cvforwin::core::validate_inspection_result(valid_result(), kResultSize).has_value());
}

TEST_CASE("error info validation enforces size, reserved, and buffer rules", "[core][validation][error]")
{
    SECTION("struct size mismatch")
    {
        cvforwin::core::ErrorInfoView view = valid_error();
        view.struct_size = 0u;
        const auto failure = cvforwin::core::validate_error_info(view, kErrorSize);
        REQUIRE(failure.has_value());
        CHECK(failure->status == Status::invalid_argument);
        CHECK(failure->code == ErrorCode::error_info_invalid);
    }

    SECTION("nonzero reserved")
    {
        cvforwin::core::ErrorInfoView view = valid_error();
        view.reserved[2] = 1u;
        const auto failure = cvforwin::core::validate_error_info(view, kErrorSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::reserved_not_zero);
    }

    SECTION("NULL message with nonzero capacity")
    {
        cvforwin::core::ErrorInfoView view = valid_error();
        view.message_present = false;
        view.message_capacity = 1024u;
        const auto failure = cvforwin::core::validate_error_info(view, kErrorSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::buffer_argument_invalid);
    }

    SECTION("NULL message with zero capacity is legal")
    {
        cvforwin::core::ErrorInfoView view = valid_error();
        view.message_present = false;
        view.message_capacity = 0u;
        CHECK_FALSE(cvforwin::core::validate_error_info(view, kErrorSize).has_value());
    }
}

TEST_CASE("init options validation follows the documented order and rules", "[core][validation][init]")
{
    SECTION("struct size mismatch is invalid argument")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.struct_size = 0u;
        const auto failure = cvforwin::core::validate_init_options(view, kInitSize);
        REQUIRE(failure.has_value());
        CHECK(failure->status == Status::invalid_argument);
        CHECK(failure->code == ErrorCode::struct_size_mismatch);
    }

    SECTION("abi version mismatch is an ABI mismatch")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.abi_version = 2u;
        const auto failure = cvforwin::core::validate_init_options(view, kInitSize);
        REQUIRE(failure.has_value());
        CHECK(failure->status == Status::abi_mismatch);
        CHECK(failure->code == ErrorCode::abi_version_mismatch);
    }

    SECTION("nonzero reserved is invalid argument")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.reserved[7] = 0xFFFFFFFFu;
        const auto failure = cvforwin::core::validate_init_options(view, kInitSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::reserved_not_zero);
    }

    SECTION("unknown flag bits are invalid argument")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.flags = 0x80000000u;
        const auto failure = cvforwin::core::validate_init_options(view, kInitSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::unknown_flags);
    }

    SECTION("callback logging requires a callback")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.flags = cvforwin::core::kInitFlagCallbackLogging;
        view.log_callback_present = false;
        const auto failure = cvforwin::core::validate_init_options(view, kInitSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::missing_log_callback);
    }

    SECTION("callback logging with a callback is accepted")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.flags = cvforwin::core::kInitFlagCallbackLogging;
        view.log_callback_present = true;
        CHECK_FALSE(cvforwin::core::validate_init_options(view, kInitSize).has_value());
    }

    SECTION("relative config root is rejected")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.config_root = "relative/config";
        const auto failure = cvforwin::core::validate_init_options(view, kInitSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::path_not_absolute);
    }

    SECTION("empty output root is rejected")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.output_root = "";
        const auto failure = cvforwin::core::validate_init_options(view, kInitSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::text_argument_invalid);
    }

    SECTION("embedded NUL in a root is rejected")
    {
        cvforwin::core::InitOptionsView view = valid_options();
        view.config_root = std::string_view("/opt/cvf\0orwin", 14u);
        const auto failure = cvforwin::core::validate_init_options(view, kInitSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::text_argument_invalid);
    }
}

TEST_CASE("inspection request validation enforces identifiers and JSON bounds", "[core][validation][request]")
{
    SECTION("empty recipe id is rejected")
    {
        cvforwin::core::InspectionRequestView view = valid_request();
        view.recipe_id = "";
        const auto failure = cvforwin::core::validate_inspection_request(view, kRequestSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::text_argument_invalid);
    }

    SECTION("over-long request id is rejected")
    {
        const std::string over_long(cvforwin::core::kMaxRequestIdBytes + 1u, 'x');
        cvforwin::core::InspectionRequestView view = valid_request();
        view.request_id = over_long;
        const auto failure = cvforwin::core::validate_inspection_request(view, kRequestSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::text_argument_too_long);
    }

    SECTION("an absent input JSON is legal")
    {
        cvforwin::core::InspectionRequestView view = valid_request();
        view.input_json_present = false;
        CHECK_FALSE(cvforwin::core::validate_inspection_request(view, kRequestSize).has_value());
    }

    SECTION("invalid UTF-8 input JSON is rejected")
    {
        cvforwin::core::InspectionRequestView view = valid_request();
        view.input_json_present = true;
        view.input_json = "\xC3\x28";
        const auto failure = cvforwin::core::validate_inspection_request(view, kRequestSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::text_argument_invalid);
    }

    SECTION("over-long input JSON is rejected")
    {
        const std::string over_long(cvforwin::core::kMaxInputJsonBytes + 1u, 'x');
        cvforwin::core::InspectionRequestView view = valid_request();
        view.input_json_present = true;
        view.input_json = over_long;
        const auto failure = cvforwin::core::validate_inspection_request(view, kRequestSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::text_argument_too_long);
    }

    SECTION("reserved words must be zero")
    {
        cvforwin::core::InspectionRequestView view = valid_request();
        view.reserved[0] = 1u;
        const auto failure = cvforwin::core::validate_inspection_request(view, kRequestSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::reserved_not_zero);
    }
}

TEST_CASE("inspection result validation enforces the v1 buffer capacities", "[core][validation][result]")
{
    SECTION("required output buffers must be present at full capacity")
    {
        cvforwin::core::InspectionResultView view = valid_result();
        view.output_json_present = false;
        view.output_json_capacity = 0u;
        const auto failure = cvforwin::core::validate_inspection_result(view, kResultSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::buffer_argument_invalid);
    }

    SECTION("a short output buffer is rejected")
    {
        cvforwin::core::InspectionResultView view = valid_result();
        view.image_path_capacity = cvforwin::core::kRequiredImagePathCapacity - 1u;
        const auto failure = cvforwin::core::validate_inspection_result(view, kResultSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::buffer_argument_invalid);
    }

    SECTION("an absent error message buffer is legal")
    {
        cvforwin::core::InspectionResultView view = valid_result();
        view.error_message_present = false;
        view.error_message_capacity = 0u;
        CHECK_FALSE(cvforwin::core::validate_inspection_result(view, kResultSize).has_value());
    }

    SECTION("a NULL error message with capacity is rejected")
    {
        cvforwin::core::InspectionResultView view = valid_result();
        view.error_message_present = false;
        view.error_message_capacity = cvforwin::core::kRequiredErrorMessageCapacity;
        const auto failure = cvforwin::core::validate_inspection_result(view, kResultSize);
        REQUIRE(failure.has_value());
        CHECK(failure->code == ErrorCode::buffer_argument_invalid);
    }
}

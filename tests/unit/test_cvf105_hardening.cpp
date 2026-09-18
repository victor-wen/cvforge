// Developer-owned regression tests for the CVF-105 hardening change.
//
// Independent, hash-pinned black-box coverage lives in tests/unit/cvf105_*.
// This file is the developer's own coverage for the four bounded fixes:
//   FR-024 canonical VID/PID configuration and identity matching,
//   FR-025 C ABI pointer-length/capacity pairs,
//   FR-026 atomic diagnostics warning bits,
//   FR-027 single bounded, pre-serialized result payload.
//
// It is deterministic, hardware-free, and creates all fixture state under the
// system temp root at run time.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "camera/camera_backend.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"
#include "core/warnings.h"
#include "cvforwin/cvf_api.h"
#include "diagnostics/diagnostics.h"
#include "recipes/config.h"
#include "runtime/context.h"

#include <opencv2/core.hpp>

namespace {

namespace camera = cvforwin::camera;
namespace core = cvforwin::core;
namespace diag = cvforwin::diagnostics;
namespace recipes = cvforwin::recipes;
namespace rt = cvforwin::runtime;
namespace fs = std::filesystem;

using Json = nlohmann::json;

fs::path scratch_dir(std::string_view label)
{
    const auto stamp = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const fs::path path =
        fs::temp_directory_path() / ("cvf105dev_" + std::string(label) + "_" + std::to_string(stamp));
    std::error_code error;
    fs::remove_all(path, error);
    fs::create_directories(path, error);
    REQUIRE(fs::is_directory(path));
    return path;
}

void write_text_file(const fs::path& path, std::string_view text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
}

Json config_json(std::string_view backend, std::string_view device_path, std::string_view vendor_id,
                 std::string_view product_id, std::string_view friendly_name)
{
    Json config = Json::object();
    config["schema_version"] = 1;
    config["camera"] = Json::object();
    config["camera"]["backend"] = std::string(backend);
    config["camera"]["device_path"] = std::string(device_path);
    config["camera"]["vendor_id"] = std::string(vendor_id);
    config["camera"]["product_id"] = std::string(product_id);
    config["camera"]["friendly_name"] = std::string(friendly_name);
    config["base_capture"] = Json::object();
    config["base_capture"]["width"] = 16;
    config["base_capture"]["height"] = 12;
    config["base_capture"]["frame_rate"] = 30.0;
    config["base_capture"]["pixel_format"] = "bgr8";
    config["logging"] = Json::object();
    config["logging"]["level"] = "info";
    config["logging"]["max_file_bytes"] = 1048576;
    config["logging"]["max_files"] = 2;
    config["retention"] = Json::object();
    config["retention"]["max_age_days"] = 30;
    config["retention"]["max_total_bytes"] = 1073741824;
    return config;
}

Json recipe_json()
{
    Json recipe = Json::object();
    recipe["schema_version"] = 1;
    recipe["recipe_id"] = "example";
    recipe["algorithm"] = "example.threshold";
    recipe["parameters"] = Json::object();
    recipe["parameters"]["threshold"] = 128;
    recipe["parameters"]["min_pass_ratio"] = 0.0;
    recipe["capture"] = Json::object();
    recipe["capture"]["width"] = 16;
    recipe["capture"]["height"] = 12;
    recipe["capture"]["frame_rate"] = 15.0;
    recipe["capture"]["pixel_format"] = "bgr8";
    recipe["capture"]["settle_frames"] = 0;
    recipe["artifacts"] = Json::object();
    recipe["artifacts"]["save_policy"] = "never";
    recipe["artifacts"]["required"] = false;
    return recipe;
}

void write_synthetic_root(const fs::path& root)
{
    write_text_file(root / "cvforwin.json",
                    config_json("synthetic", "", "0000", "0000", "Synthetic camera").dump(2));
    write_text_file(root / "recipes" / "example.json", recipe_json().dump(2));
}

std::atomic<unsigned long long> g_callback_calls{0};

void throwing_callback(std::uint32_t /*level*/, const char* /*message*/, void* /*user_data*/)
{
    g_callback_calls.fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("cvf105 developer contained callback failure");
}

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

cvf_inspection_request_v1 inspection_request(const std::string& recipe_id, const std::string& request_id)
{
    cvf_inspection_request_v1 request{};
    request.struct_size = sizeof(cvf_inspection_request_v1);
    request.abi_version = CVF_ABI_VERSION_V1;
    request.recipe_id_utf8 = recipe_id.c_str();
    request.recipe_id_utf8_bytes = static_cast<std::uint32_t>(recipe_id.size());
    request.request_id_utf8 = request_id.c_str();
    request.request_id_utf8_bytes = static_cast<std::uint32_t>(request_id.size());
    return request;
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* FR-024                                                                     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 dev FR-024: canonicalize_hex4 canonicalizes and rejects", "[cvf-105-dev][FR-024]")
{
    const auto canonical = recipes::canonicalize_hex4("1A2b");
    REQUIRE(canonical.has_value());
    CHECK(canonical.value() == "1a2b");
    CHECK(canonical.value().size() == 4u);

    for (const char* invalid : {"", "1a2", "1a2b3", "1a2g", "0x1a", "1a2b ", " 1a2b"}) {
        const auto rejected = recipes::canonicalize_hex4(invalid);
        INFO("input: [" << invalid << "]");
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.failure().status == core::Status::config_error);
        CHECK(rejected.failure().code == core::ErrorCode::config_value_invalid);
    }
}

TEST_CASE("CVF-105 dev FR-024: config load canonicalizes and identity matches mixed case",
          "[cvf-105-dev][FR-024]")
{
    const fs::path root = scratch_dir("config");
    write_text_file(root / "cvforwin.json", config_json("uvc", "", "1A2B", "0C3D", "Exact Name").dump(2));

    const auto loaded = recipes::load_global_config(root, root / "out");
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().camera.vendor_id == "1a2b");
    CHECK(loaded.value().camera.product_id == "0c3d");
    CHECK(loaded.value().camera.friendly_name == "Exact Name");

    const camera::CameraSelector selector{loaded.value().camera.device_path, loaded.value().camera.vendor_id,
                                          loaded.value().camera.product_id, loaded.value().camera.friendly_name};
    const std::vector<camera::CameraDescriptor> candidates{
        camera::CameraDescriptor{"uvc", "usb/probe", "1A2B", "0c3d", "Exact Name"}};
    const auto resolved = camera::resolve_identity(candidates, selector);
    REQUIRE(resolved.has_value());

    const std::vector<camera::CameraDescriptor> paths{
        camera::CameraDescriptor{"uvc", "Path/Alpha", "1a2b", "0c3d", "Cam"}};

    /* device_path stays an exact byte comparison. */
    CHECK_FALSE(camera::resolve_identity(paths, camera::CameraSelector{"path/alpha", "1a2b", "0c3d", ""}).has_value());
    CHECK(camera::resolve_identity(paths, camera::CameraSelector{"Path/Alpha", "1A2B", "0c3d", ""}).has_value());

    /* A non-hexadecimal selector never matches a canonical descriptor. */
    const auto bad = camera::resolve_identity(paths, camera::CameraSelector{"", "1a2g", "0c3d", ""});
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.failure().code == core::ErrorCode::camera_not_found);
}

/* ------------------------------------------------------------------------- */
/* FR-026                                                                     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 dev FR-026: diagnostics warning bits survive concurrent writers", "[cvf-105-dev][FR-026]")
{
    const fs::path root = scratch_dir("warnings");

    diag::DiagnosticsConfig config;
    config.level = core::LogLevel::info;
    config.max_file_bytes = 1048576;
    config.max_files = 2;
    config.log_dir = root / "logs";
    config.enable_file_sink = false;

    diag::CallbackBinding binding;
    binding.fn = &throwing_callback;
    binding.user_data = nullptr;

    auto created = diag::Diagnostics::create(config, binding);
    REQUIRE(created.has_value());
    std::unique_ptr<diag::Diagnostics> diagnostics = std::move(created).value();

    CHECK(diagnostics->warnings() == 0u);

    g_callback_calls.store(0u, std::memory_order_relaxed);
    constexpr int k_threads = 3;
    constexpr int k_entries = 300;
    std::vector<std::thread> writers;
    writers.reserve(k_threads);
    for (int index = 0; index < k_threads; ++index) {
        writers.emplace_back([&diagnostics] {
            for (int entry = 0; entry < k_entries; ++entry) {
                diagnostics->log(core::LogLevel::info, "cvf105 developer warning");
            }
        });
    }
    for (std::thread& writer : writers) {
        writer.join();
    }

    CHECK(g_callback_calls.load(std::memory_order_relaxed) ==
          static_cast<unsigned long long>(k_threads) * static_cast<unsigned long long>(k_entries));
    CHECK((diagnostics->warnings() &
           static_cast<std::uint32_t>(core::WarningFlags::warning_log_sink_failed)) != 0u);

    diagnostics->clear_warnings();
    CHECK(diagnostics->warnings() == 0u);
}

/* ------------------------------------------------------------------------- */
/* FR-027                                                                     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 dev FR-027: serialize_result_payload enforces the bound verbatim",
          "[cvf-105-dev][FR-027]")
{
    CHECK(rt::k_max_result_payload_bytes == 65535u);

    const auto make_payload = [](std::size_t target) {
        Json payload = Json::object();
        payload["p"] = std::string(target - 8u, 'a');
        REQUIRE(payload.dump().size() == target);
        return payload;
    };

    const Json at_bound_payload = make_payload(rt::k_max_result_payload_bytes);
    const auto at_bound = rt::serialize_result_payload(at_bound_payload);
    REQUIRE(at_bound.has_value());
    CHECK(at_bound.value().size() == rt::k_max_result_payload_bytes);
    CHECK(at_bound.value() == at_bound_payload.dump());

    const auto over_bound = rt::serialize_result_payload(make_payload(rt::k_max_result_payload_bytes + 1u));
    REQUIRE_FALSE(over_bound.has_value());
    CHECK(over_bound.failure().status == core::Status::buffer_too_small);
    CHECK(over_bound.failure().code == core::ErrorCode::runtime_result_too_large);
}

TEST_CASE("CVF-105 dev FR-027: runtime output_text is the single canonical payload",
          "[cvf-105-dev][FR-027]")
{
    const fs::path root = scratch_dir("output_text");
    write_synthetic_root(root);

    rt::RuntimeOptions options;
    options.config_root = root;
    options.output_root = root / "out";
    options.enable_file_logging = false;
    options.enable_callback_logging = false;

    auto created = rt::Context::create(std::move(options));
    REQUIRE(created.has_value());
    std::unique_ptr<rt::Context> context = std::move(created).value();

    rt::InspectionRequest request;
    request.recipe_id = "example";
    request.request_id = "dev-fr027";
    request.timeout_ms = rt::TimeoutMs(0u);

    const auto inspected = context->inspect(request);
    REQUIRE(inspected.has_value());
    const rt::InspectionOutcome& outcome = inspected.value();
    CHECK(outcome.status == core::Status::ok);
    REQUIRE_FALSE(outcome.output_text.empty());
    CHECK(outcome.output_text == outcome.output_json.dump());
    CHECK(outcome.output_text.size() <= rt::k_max_result_payload_bytes);
    CHECK(Json::parse(outcome.output_text) == outcome.output_json);
}

/* ------------------------------------------------------------------------- */
/* FR-025                                                                     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 dev FR-025: C ABI input_json and result pointer-length pairs",
          "[cvf-105-dev][FR-025]")
{
    const fs::path root = scratch_dir("abi_pairs");
    write_synthetic_root(root);
    const std::string config_root = root.string();
    const std::string output_root = (root / "out").string();

    cvf_init_options_v1 options{};
    options.struct_size = sizeof(cvf_init_options_v1);
    options.abi_version = CVF_ABI_VERSION_V1;
    options.config_root_utf8 = config_root.c_str();
    options.config_root_utf8_bytes = static_cast<std::uint32_t>(config_root.size());
    options.output_root_utf8 = output_root.c_str();
    options.output_root_utf8_bytes = static_cast<std::uint32_t>(output_root.size());

    cvf_context* context = nullptr;
    ErrorBuffer error;
    REQUIRE(cvf_initialize(&options, &context, &error.info) == CVF_STATUS_OK);
    REQUIRE(context != nullptr);

    const std::string recipe_id = "example";
    const std::string request_id = "dev-fr025";

    SECTION("input_json NULL with a nonzero length is INVALID_ARGUMENT")
    {
        cvf_inspection_request_v1 request = inspection_request(recipe_id, request_id);
        request.input_json_utf8 = nullptr;
        request.input_json_utf8_bytes = 3u;
        ResultBuffer buffer;

        CHECK(cvf_inspect(context, &request, &buffer.result) == CVF_STATUS_INVALID_ARGUMENT);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }

    SECTION("input_json non-NULL with zero length is absent")
    {
        cvf_inspection_request_v1 request = inspection_request(recipe_id, request_id);
        request.input_json_utf8 = "{}";
        request.input_json_utf8_bytes = 0u;
        ResultBuffer buffer;

        CHECK(cvf_inspect(context, &request, &buffer.result) == CVF_STATUS_OK);
        CHECK(buffer.result.status == CVF_STATUS_OK);
    }

    SECTION("output_json NULL with a nonzero capacity is INVALID_ARGUMENT")
    {
        cvf_inspection_request_v1 request = inspection_request(recipe_id, request_id);
        request.input_json_utf8 = "{}";
        request.input_json_utf8_bytes = 2u;
        ResultBuffer buffer;
        buffer.result.output_json = nullptr;
        buffer.result.output_json_capacity = CVF_RESULT_JSON_REQUIRED_CAPACITY;

        CHECK(cvf_inspect(context, &request, &buffer.result) == CVF_STATUS_INVALID_ARGUMENT);
        CHECK(buffer.result.verdict == CVF_VERDICT_NOT_EVALUATED);
    }

    SECTION("output_json is copied verbatim with matching byte counts")
    {
        cvf_inspection_request_v1 request = inspection_request(recipe_id, request_id);
        request.input_json_utf8 = "{}";
        request.input_json_utf8_bytes = 2u;
        ResultBuffer buffer;

        REQUIRE(cvf_inspect(context, &request, &buffer.result) == CVF_STATUS_OK);
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

    cvf_shutdown(context);
}

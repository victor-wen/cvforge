/*
 * Developer regression tests for the CVF-006 runtime orchestrator.
 *
 * Focused coverage of the lifecycle contract: rollback-safe creation, the
 * valued technical-failure outcome, atomic reload with old-snapshot
 * preservation, and the optional-artifact warning path. The independent
 * cvf006_* suite remains the acceptance evidence; these cases guard the same
 * behavior from the implementation side.
 */

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "camera/test_backends/synthetic_camera_backend.h"
#include "core/status.h"
#include "core/warnings.h"
#include "runtime/context.h"

namespace {

namespace cam = cvforwin::camera;
namespace core = cvforwin::core;
namespace rt = cvforwin::runtime;

class TempDir {
public:
    explicit TempDir(std::string tag)
    {
        static std::atomic<std::uint64_t> counter{0u};
        path_ = std::filesystem::temp_directory_path() /
                ("cvf006_dev_" + std::move(tag) + "_" + std::to_string(counter.fetch_add(1u)));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        REQUIRE_FALSE(error);
        REQUIRE(std::filesystem::create_directories(path_, error));
        REQUIRE_FALSE(error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_text(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << text;
    REQUIRE(stream.good());
}

nlohmann::json synthetic_config()
{
    return nlohmann::json{
        {"schema_version", 1},
        {"camera",
         nlohmann::json{{"backend", "synthetic"},
                        {"device_path", "synthetic0"},
                        {"vendor_id", "0000"},
                        {"product_id", "0000"},
                        {"friendly_name", "Synthetic camera"}}},
        {"base_capture",
         nlohmann::json{{"width", 16}, {"height", 12}, {"frame_rate", 30.0}, {"pixel_format", "bgr8"}}},
        {"logging", nlohmann::json{{"level", "info"}, {"max_file_bytes", 1048576}, {"max_files", 2}}},
        {"retention", nlohmann::json{{"max_age_days", 30}, {"max_total_bytes", 1073741824}}},
    };
}

nlohmann::json recipe_document(const std::string& recipe_id, int threshold, double min_pass_ratio,
                               const std::string& save_policy = "never", bool required = false)
{
    return nlohmann::json{
        {"schema_version", 1},
        {"recipe_id", recipe_id},
        {"algorithm", "example.threshold"},
        {"parameters", nlohmann::json{{"threshold", threshold}, {"min_pass_ratio", min_pass_ratio}}},
        {"capture",
         nlohmann::json{{"width", 16},
                        {"height", 12},
                        {"frame_rate", 15.0},
                        {"pixel_format", "bgr8"},
                        {"settle_frames", 0}}},
        {"artifacts", nlohmann::json{{"save_policy", save_policy}, {"required", required}}},
    };
}

void write_config(const std::filesystem::path& config_root)
{
    write_text(config_root / "cvforwin.json", synthetic_config().dump(2));
}

void write_recipe(const std::filesystem::path& config_root, const std::string& filename,
                  const nlohmann::json& recipe)
{
    std::error_code error;
    std::filesystem::create_directories(config_root / "recipes", error);
    REQUIRE_FALSE(error);
    write_text(config_root / "recipes" / filename, recipe.dump(2));
}

rt::RuntimeOptions options_for(const std::filesystem::path& config_root,
                               const std::filesystem::path& output_root)
{
    rt::RuntimeOptions options;
    options.config_root = config_root;
    options.output_root = output_root;
    options.enable_file_logging = false;
    return options;
}

const char* kRecipeId = "example.pass";
const char* kRequestId = "dev-request";

rt::InspectionRequest request_for(const std::string& recipe_id)
{
    rt::InspectionRequest request;
    request.recipe_id = recipe_id;
    request.request_id = kRequestId;
    request.timeout_ms = 5000u;
    return request;
}

}  // namespace

TEST_CASE("CVF-006 developer: a missing configuration root fails creation cleanly", "[cvf-006][dev]")
{
    TempDir scratch("missing_config");
    TempDir output("missing_output");

    auto created = rt::Context::create(options_for(scratch.path() / "does_not_exist", output.path()));

    REQUIRE_FALSE(created.has_value());
    CHECK(created.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-006 developer: inspect returns a valued outcome and honors save_policy never",
          "[cvf-006][dev]")
{
    TempDir config("config");
    TempDir output("output");
    write_config(config.path());
    write_recipe(config.path(), "pass.json", recipe_document(kRecipeId, 128, 0.0));

    auto backend = std::make_shared<cam::SyntheticCameraBackend>(cam::SyntheticCameraConfig{
        .descriptor = cam::CameraDescriptor{.backend_key = "synthetic",
                                            .device_path = "synthetic0",
                                            .vendor_id = "0000",
                                            .product_id = "0000",
                                            .friendly_name = "Synthetic camera"},
        .width = 16,
        .height = 12,
    });
    auto options = options_for(config.path(), output.path());
    options.camera_override = backend;

    auto created = rt::Context::create(std::move(options));
    REQUIRE(created.has_value());
    std::unique_ptr<rt::Context> context = std::move(created).value();

    auto outcome = context->inspect(request_for(kRecipeId));
    REQUIRE(outcome.has_value());
    CHECK(outcome.value().status == core::Status::ok);
    CHECK(outcome.value().verdict == core::Verdict::pass);
    CHECK(outcome.value().error_code == core::ErrorCode::none);
    CHECK(outcome.value().image_path.empty());
    CHECK(context->warning_flags() == 0u);

    const nlohmann::json& measurements = outcome.value().output_json;
    REQUIRE(measurements.is_object());
    CHECK(measurements.at("total_pixels") == 192);
    CHECK(backend->capture_call_count == 1);

    // Unknown recipes are technical errors with NOT_EVALUATED and cleared JSON.
    auto missing = context->inspect(request_for("no.such.recipe"));
    REQUIRE(missing.has_value());
    CHECK(missing.value().status == core::Status::recipe_not_found);
    CHECK(missing.value().verdict == core::Verdict::not_evaluated);
    CHECK(missing.value().output_json.empty());
    CHECK(missing.value().image_path.empty());
}

TEST_CASE("CVF-006 developer: reload is atomic and keeps the old snapshot on rejection",
          "[cvf-006][dev]")
{
    TempDir config("reload_config");
    TempDir output("reload_output");
    write_config(config.path());
    write_recipe(config.path(), "pass.json", recipe_document(kRecipeId, 128, 0.0));

    auto backend = std::make_shared<cam::SyntheticCameraBackend>(cam::SyntheticCameraConfig{
        .descriptor = cam::CameraDescriptor{.backend_key = "synthetic",
                                            .device_path = "synthetic0",
                                            .vendor_id = "0000",
                                            .product_id = "0000",
                                            .friendly_name = "Synthetic camera"},
        .width = 16,
        .height = 12,
    });
    auto options = options_for(config.path(), output.path());
    options.camera_override = backend;
    auto created = rt::Context::create(std::move(options));
    REQUIRE(created.has_value());
    std::unique_ptr<rt::Context> context = std::move(created).value();

    CHECK_FALSE(context->inspect(request_for("added")).value().status == core::Status::ok);

    write_recipe(config.path(), "added.json", recipe_document("added", 128, 0.0));
    REQUIRE(context->reload_recipes().has_value());
    CHECK(context->inspect(request_for("added")).value().status == core::Status::ok);

    // An invalid candidate must not disturb the published snapshot.
    write_recipe(config.path(), "broken.json",
                 recipe_document("broken", 999, 0.0)); /* threshold outside 0..255 */
    auto rejected = context->reload_recipes();
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.failure().status == core::Status::config_error);
    CHECK(context->inspect(request_for("added")).value().status == core::Status::ok);
}

TEST_CASE("CVF-006 developer: an optional save failure warns without changing the verdict",
          "[cvf-006][dev]")
{
    TempDir config("save_config");
    TempDir output("save_output");
    write_config(config.path());
    write_recipe(config.path(), "always.json", recipe_document(kRecipeId, 128, 0.0, "always", false));

    auto backend = std::make_shared<cam::SyntheticCameraBackend>(cam::SyntheticCameraConfig{
        .descriptor = cam::CameraDescriptor{.backend_key = "synthetic",
                                            .device_path = "synthetic0",
                                            .vendor_id = "0000",
                                            .product_id = "0000",
                                            .friendly_name = "Synthetic camera"},
        .width = 16,
        .height = 12,
    });
    auto options = options_for(config.path(), output.path());
    options.camera_override = backend;
    auto created = rt::Context::create(std::move(options));
    REQUIRE(created.has_value());
    std::unique_ptr<rt::Context> context = std::move(created).value();

    // Replace the output root with a regular file after initialization so any
    // persistence below it must fail while the computed verdict stays valid.
    std::error_code error;
    std::filesystem::remove_all(output.path(), error);
    REQUIRE_FALSE(error);
    write_text(output.path(), "blocked");

    auto outcome = context->inspect(request_for(kRecipeId));
    REQUIRE(outcome.has_value());
    CHECK(outcome.value().status == core::Status::ok);
    CHECK(outcome.value().verdict == core::Verdict::pass);
    CHECK(outcome.value().image_path.empty());
    CHECK((outcome.value().warning_flags & static_cast<std::uint32_t>(core::warning_image_save_failed)) != 0u);
}

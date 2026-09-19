/*
 * Developer-owned CVF-106 tests.
 *
 * These cases are authored by the developer (not the independent test owner).
 * They cover:
 *   1. template.match parameter validation (missing/unknown/wrong-typed/
 *      out-of-range keys) through the registered algorithm.
 *   2. A golden-image/tolerance match: a template embedded at a known location
 *      must score 1.0 (tolerance 1e-6) at the exact global coordinates, and the
 *      result must agree with an independent cv::matchTemplate computation
 *      (tolerance 1e-9). A frame without the template must fail below threshold
 *      with a deterministic template_mismatch defect.
 *   3. The shipped example recipe: config/examples/recipes/template.match.json
 *      and config/examples/assets/tmpl.asymmetric.png load through the catalog,
 *      are prepared once, and inspect the shipped asset correctly.
 *   4. A runtime regression for the CVF-106 context.cpp change: an injected
 *      camera_override is authoritative for descriptor selection even when the
 *      config selector does not match the injected backend.
 */

#include "algorithms/compiled_algorithms.h"
#include "algorithms/template_match.h"

#include "camera/captured_frame.h"
#include "camera/test_backends/file_camera_backend.h"

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

#include "inspection/algorithm.h"
#include "inspection/registry.h"

#include "recipes/recipe.h"
#include "recipes/recipe_catalog.h"

#include "runtime/context.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef CVFORWIN_CONFIG_EXAMPLES_DIR
#error "CVFORWIN_CONFIG_EXAMPLES_DIR must name the config/examples directory"
#endif

namespace {

namespace core = cvforwin::core;
namespace insp = cvforwin::inspection;
namespace alg = cvforwin::algorithms;
namespace cam = cvforwin::camera;
namespace recipes = cvforwin::recipes;
namespace rt = cvforwin::runtime;

using Json = nlohmann::json;

constexpr std::string_view k_template_key = "template.match";
constexpr std::string_view k_asset_key = "tmpl";
constexpr std::string_view k_example_reference = "tmpl.asymmetric.png";
constexpr int k_frame_width = 64;
constexpr int k_frame_height = 48;
constexpr double k_passing_threshold = 0.8;

// --- temporary directories and file helpers ---------------------------------

class TempDir {
public:
    explicit TempDir(std::string_view tag)
    {
        namespace fs = std::filesystem;
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        path_ = fs::temp_directory_path() / ("cvf106_dev_" + std::string(tag) + "_" + unique);
        std::error_code error;
        fs::remove_all(path_, error);
        if (!fs::create_directories(path_, error)) {
            throw std::runtime_error("cvf106 developer: cannot create temp directory");
        }
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

void write_text(const std::filesystem::path& file, std::string_view text)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    REQUIRE(stream.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

// --- deterministic fixtures -------------------------------------------------

// 7x5 single-channel template, asymmetric in both axes, so a transposed or
// mirrored match cannot score as high as the exact placement.
cv::Mat asymmetric_template()
{
    static const unsigned char k_pixels[5][7] = {
        {12, 44, 91, 160, 231, 5, 77},
        {203, 19, 68, 142, 210, 188, 33},
        {61, 130, 250, 30, 101, 222, 149},
        {220, 111, 14, 181, 56, 9, 244},
        {3, 90, 175, 25, 200, 64, 128},
    };
    cv::Mat tmpl(5, 7, CV_8UC1);
    for (int y = 0; y < tmpl.rows; ++y) {
        for (int x = 0; x < tmpl.cols; ++x) {
            tmpl.at<unsigned char>(y, x) = k_pixels[y][x];
        }
    }
    return tmpl;
}

cv::Mat frame_with_template(int width, int height, const cv::Mat& tmpl, int origin_x, int origin_y)
{
    REQUIRE(tmpl.type() == CV_8UC1);
    REQUIRE(origin_x >= 0);
    REQUIRE(origin_y >= 0);
    REQUIRE(origin_x + tmpl.cols <= width);
    REQUIRE(origin_y + tmpl.rows <= height);

    cv::Mat frame(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int value = (x * 7 + y * 13 + 21) % 256;
            frame.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<unsigned char>(value),
                                                  static_cast<unsigned char>((value + 40) % 256),
                                                  static_cast<unsigned char>((value + 90) % 256));
        }
    }
    for (int y = 0; y < tmpl.rows; ++y) {
        for (int x = 0; x < tmpl.cols; ++x) {
            const unsigned char value = tmpl.at<unsigned char>(y, x);
            frame.at<cv::Vec3b>(origin_y + y, origin_x + x) = cv::Vec3b(value, value, value);
        }
    }
    return frame;
}

cv::Mat frame_without_template(int width, int height)
{
    cv::Mat frame(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int value = (x * 5 + y * 11 + 3) % 256;
            frame.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<unsigned char>(value),
                                                  static_cast<unsigned char>((value + 70) % 256),
                                                  static_cast<unsigned char>((value + 140) % 256));
        }
    }
    return frame;
}

cam::CapturedFrame captured_frame(cv::Mat pixels)
{
    cam::CapturedFrame frame{};
    frame.metadata.width = static_cast<std::uint32_t>(pixels.cols);
    frame.metadata.height = static_cast<std::uint32_t>(pixels.rows);
    frame.metadata.pixel_format = cam::PixelFormat::bgr8;
    frame.pixels = std::move(pixels);
    return frame;
}

std::vector<std::uint8_t> encode_png(const cv::Mat& image)
{
    std::vector<unsigned char> buffer;
    REQUIRE(cv::imencode(".png", image, buffer));
    return std::vector<std::uint8_t>(buffer.begin(), buffer.end());
}

Json template_parameters(int x, int y, int width, int height, double threshold)
{
    return Json{{"template_asset", std::string(k_asset_key)},
                {"roi", Json::array({x, y, width, height})},
                {"method", "ccoeff_normed"},
                {"threshold", threshold}};
}

Json template_recipe(const std::string& recipe_id, const Json& parameters, const Json& assets)
{
    Json recipe{
        {"schema_version", 1},
        {"recipe_id", recipe_id},
        {"algorithm", std::string(k_template_key)},
        {"parameters", parameters},
        {"capture", Json{{"width", k_frame_width}, {"height", k_frame_height}, {"frame_rate", 15.0}, {"pixel_format", "bgr8"}, {"settle_frames", 0}}},
        {"artifacts", Json{{"save_policy", "never"}, {"required", false}}},
    };
    if (!assets.is_null()) {
        recipe["assets"] = assets;
    }
    return recipe;
}

Json runtime_config()
{
    return Json{
        {"schema_version", 1},
        {"camera", Json{{"backend", "file"}, {"device_path", ""}, {"vendor_id", "FFFF"}, {"product_id", "EEEE"}, {"friendly_name", "configured-not-injected"}}},
        {"base_capture", Json{{"width", k_frame_width}, {"height", k_frame_height}, {"frame_rate", 30.0}, {"pixel_format", "bgr8"}}},
        {"logging", Json{{"level", "info"}, {"max_file_bytes", 1048576}, {"max_files", 2}}},
        {"retention", Json{{"max_age_days", 30}, {"max_total_bytes", 1073741824}}},
    };
}

// --- registry / dispatch helpers -------------------------------------------

class Registry {
public:
    Registry()
    {
        auto added = alg::register_compiled_algorithms(registry_);
        REQUIRE(added.has_value());
    }

    const insp::IInspectionAlgorithm& algorithm() const
    {
        auto found = registry_.find(k_template_key);
        REQUIRE(found.has_value());
        return *found.value();
    }

private:
    insp::AlgorithmRegistry registry_{};
};

insp::AlgorithmAssetBundle asset_bundle(std::vector<std::uint8_t> bytes)
{
    insp::AlgorithmAssetBundle bundle;
    insp::AlgorithmAsset asset;
    asset.key = std::string(k_asset_key);
    asset.reference = std::string(k_example_reference);
    asset.bytes = std::move(bytes);
    bundle.assets.push_back(std::move(asset));
    return bundle;
}

core::Result<insp::AlgorithmResult> inspect_prepared(const insp::IPreparedAlgorithm& prepared,
                                                     const cam::CapturedFrame& frame,
                                                     const Json& parameters)
{
    const std::optional<Json> input = std::nullopt;
    const insp::AlgorithmRequest request{frame, parameters, input, core::Deadline::from_timeout_ms(5000)};
    return insp::dispatch(prepared, request);
}

std::string defect_kind(const insp::AlgorithmResult& result)
{
    for (const Json& defect : result.defects) {
        if (defect.is_object() && defect.contains("kind") && defect.at("kind").is_string()) {
            return defect.at("kind").get<std::string>();
        }
    }
    return "";
}

}  // namespace

TEST_CASE("CVF-106 developer: template.match parameter validation", "[cvf106][developer]")
{
    const Registry registry;
    const insp::IInspectionAlgorithm& algorithm = registry.algorithm();

    const auto expect_invalid = [&algorithm](const Json& candidate) {
        const core::Result<void> result = algorithm.validate_parameters(candidate);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.failure().status == core::Status::config_error);
        CHECK(result.failure().code == core::ErrorCode::algorithm_parameters_invalid);
    };

    SECTION("valid parameters are accepted")
    {
        CHECK(algorithm
                  .validate_parameters(template_parameters(0, 0, k_frame_width, k_frame_height,
                                                           k_passing_threshold))
                  .has_value());
    }

    SECTION("non-object parameters are rejected")
    {
        expect_invalid(Json::array());
    }

    SECTION("unknown key is rejected")
    {
        Json parameters = template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold);
        parameters["unexpected"] = 1;
        expect_invalid(parameters);
    }

    SECTION("missing method is rejected")
    {
        Json parameters = template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold);
        parameters.erase("method");
        expect_invalid(parameters);
    }

    SECTION("non-finite threshold is rejected")
    {
        Json parameters = template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold);
        parameters["threshold"] = std::numeric_limits<double>::quiet_NaN();
        expect_invalid(parameters);
    }

    SECTION("threshold above one is rejected")
    {
        expect_invalid(template_parameters(0, 0, k_frame_width, k_frame_height, 1.5));
    }

    SECTION("threshold below zero is rejected")
    {
        expect_invalid(template_parameters(0, 0, k_frame_width, k_frame_height, -0.1));
    }

    SECTION("roi with the wrong arity is rejected")
    {
        Json parameters = template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold);
        parameters["roi"] = Json::array({0, 0, k_frame_width});
        expect_invalid(parameters);
    }

    SECTION("non-integer roi entries are rejected")
    {
        Json parameters = template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold);
        parameters["roi"] = Json::array({0, 0, 64.5, k_frame_height});
        expect_invalid(parameters);
    }

    SECTION("zero roi dimension is rejected")
    {
        expect_invalid(template_parameters(0, 0, 0, k_frame_height, k_passing_threshold));
    }

    SECTION("roi beyond the compiled extent is rejected")
    {
        expect_invalid(template_parameters(0, 0, 65, k_frame_height, k_passing_threshold));
    }

    SECTION("negative roi origin is rejected")
    {
        expect_invalid(template_parameters(-1, 0, k_frame_width, k_frame_height, k_passing_threshold));
    }

    SECTION("an unsupported method is rejected")
    {
        Json parameters = template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold);
        parameters["method"] = "sqdiff";
        expect_invalid(parameters);
    }

    SECTION("a malformed asset key is rejected")
    {
        Json parameters = template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold);
        parameters["template_asset"] = "TMPL";
        expect_invalid(parameters);
    }
}

TEST_CASE("CVF-106 developer: template.match golden image match within tolerance",
          "[cvf106][developer]")
{
    const Registry registry;
    const insp::IInspectionAlgorithm& algorithm = registry.algorithm();
    const cv::Mat tmpl = asymmetric_template();
    const Json parameters =
        template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold);

    auto prepared = algorithm.prepare(parameters, asset_bundle(encode_png(tmpl)));
    REQUIRE(prepared.has_value());

    SECTION("an exact placement passes with the golden score and global coordinates")
    {
        const int origin_x = 21;
        const int origin_y = 17;
        const cam::CapturedFrame frame =
            captured_frame(frame_with_template(k_frame_width, k_frame_height, tmpl, origin_x, origin_y));

        auto result = inspect_prepared(*prepared.value(), frame, parameters);
        REQUIRE(result.has_value());
        const insp::AlgorithmResult& matched = result.value();
        REQUIRE(matched.measurements.is_object());
        const double score = matched.measurements.at("score").get<double>();

        CHECK(matched.verdict == core::Verdict::pass);
        // Golden: an exact copy scores 1.0; tolerance 1e-6.
        CHECK(score == Catch::Approx(1.0).margin(1e-6));
        CHECK(matched.measurements.at("x").get<int>() == origin_x);
        CHECK(matched.measurements.at("y").get<int>() == origin_y);
        CHECK(matched.measurements.at("width").get<int>() == tmpl.cols);
        CHECK(matched.measurements.at("height").get<int>() == tmpl.rows);
        CHECK(matched.defects.empty());

        // Independent OpenCV oracle: the algorithm result must agree to 1e-9.
        cv::Mat gray;
        cv::cvtColor(frame.pixels, gray, cv::COLOR_BGR2GRAY);
        cv::Mat scores;
        cv::matchTemplate(gray, tmpl, scores, cv::TM_CCOEFF_NORMED);
        double expected_score = 0.0;
        cv::Point expected_location;
        cv::minMaxLoc(scores, nullptr, &expected_score, nullptr, &expected_location);
        CHECK(score == Catch::Approx(expected_score).margin(1e-9));
        CHECK(matched.measurements.at("x").get<int>() == expected_location.x);
        CHECK(matched.measurements.at("y").get<int>() == expected_location.y);
    }

    SECTION("a frame without the template fails below threshold with a deterministic defect")
    {
        const cam::CapturedFrame frame = captured_frame(frame_without_template(k_frame_width, k_frame_height));
        auto result = inspect_prepared(*prepared.value(), frame, parameters);
        REQUIRE(result.has_value());
        CHECK(result.value().verdict == core::Verdict::fail);
        CHECK(result.value().measurements.at("score").get<double>() < k_passing_threshold);
        CHECK(defect_kind(result.value()) == "template_mismatch");
    }

    SECTION("an already-expired deadline fails with algorithm_deadline_exceeded")
    {
        const cam::CapturedFrame frame =
            captured_frame(frame_with_template(k_frame_width, k_frame_height, tmpl, 21, 17));
        const std::optional<Json> input = std::nullopt;
        const insp::AlgorithmRequest request{frame, parameters, input, core::Deadline::immediate()};
        const core::Result<insp::AlgorithmResult> result = insp::dispatch(*prepared.value(), request);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.failure().status == core::Status::timeout);
        CHECK(result.failure().code == core::ErrorCode::algorithm_deadline_exceeded);
    }
}

TEST_CASE("CVF-106 developer: the shipped example recipe and asset load and match",
          "[cvf106][developer]")
{
    namespace fs = std::filesystem;
    const fs::path examples = fs::path(CVFORWIN_CONFIG_EXAMPLES_DIR);

    insp::AlgorithmRegistry registry;
    REQUIRE(alg::register_compiled_algorithms(registry).has_value());

    SECTION("the example recipe document is valid and declares its asset")
    {
        auto loaded = recipes::load_recipe_file(examples / "recipes" / "template.match.json", registry);
        REQUIRE(loaded.has_value());
        CHECK(loaded.value().recipe_id == "template.match");
        CHECK(loaded.value().algorithm == "template.match");
        REQUIRE(loaded.value().assets.size() == 1u);
        CHECK(loaded.value().assets.at(0).key == "tmpl");
        CHECK(loaded.value().assets.at(0).reference == std::string(k_example_reference));
        CHECK(loaded.value().capture.width == static_cast<std::uint32_t>(k_frame_width));
        CHECK(loaded.value().capture.height == static_cast<std::uint32_t>(k_frame_height));
    }

    SECTION("the whole example catalog loads and prepares the shipped template")
    {
        auto catalog = recipes::RecipeCatalog::load(examples / "recipes", registry);
        REQUIRE(catalog.has_value());
        CHECK(catalog.value().size() >= 3u);
        CHECK(catalog.value().find("template.match").has_value());
        CHECK(catalog.value().find("example.pass").has_value());
        CHECK(catalog.value().find_prepared("example.pass").has_value());

        auto prepared = catalog.value().find_prepared("template.match");
        REQUIRE(prepared.has_value());

        auto recipe = catalog.value().find("template.match");
        REQUIRE(recipe.has_value());

        const std::vector<std::uint8_t> bytes =
            read_file_bytes(examples / "assets" / std::string(k_example_reference));
        REQUIRE_FALSE(bytes.empty());
        const cv::Mat decoded = cv::imdecode(bytes, cv::IMREAD_GRAYSCALE);
        REQUIRE_FALSE(decoded.empty());

        const int origin_x = 9;
        const int origin_y = 6;
        const cam::CapturedFrame frame =
            captured_frame(frame_with_template(k_frame_width, k_frame_height, decoded, origin_x, origin_y));
        auto result = inspect_prepared(*prepared.value(), frame, recipe.value()->parameters);
        REQUIRE(result.has_value());
        CHECK(result.value().verdict == core::Verdict::pass);
        CHECK(result.value().measurements.at("score").get<double>() == Catch::Approx(1.0).margin(1e-6));
        CHECK(result.value().measurements.at("x").get<int>() == origin_x);
        CHECK(result.value().measurements.at("y").get<int>() == origin_y);
    }
}

TEST_CASE("CVF-106 developer: an injected camera override is authoritative for descriptor selection",
          "[cvf106][developer]")
{
    namespace fs = std::filesystem;
    TempDir root("override_authority");
    TempDir output("override_authority_out");
    const cv::Mat tmpl = asymmetric_template();

    std::filesystem::create_directories(root.path() / "assets");
    REQUIRE(cv::imwrite((root.path() / "assets" / std::string(k_example_reference)).string(), tmpl));
    write_text(root.path() / "recipes" / "tmpl.json",
               template_recipe("tmpl", template_parameters(0, 0, k_frame_width, k_frame_height, k_passing_threshold),
                               Json{{"tmpl", std::string(k_example_reference)}})
                   .dump(2));
    write_text(root.path() / "cvforwin.json", runtime_config().dump(2));

    const fs::path frame_file = root.path() / "frame.png";
    REQUIRE(cv::imwrite(frame_file.string(),
                        frame_with_template(k_frame_width, k_frame_height, tmpl, 21, 17)));

    rt::RuntimeOptions options;
    options.config_root = root.path();
    options.output_root = output.path();
    options.enable_file_logging = false;
    options.enable_callback_logging = false;

    cam::FileCameraConfig backend_config{};
    backend_config.descriptor.backend_key = "file";
    backend_config.descriptor.device_path = root.path().string();
    backend_config.descriptor.friendly_name = "injected-camera";
    for (int index = 0; index < 4; ++index) {
        backend_config.frame_paths.push_back(frame_file.string());
    }
    options.camera_override = std::make_shared<cam::FileCameraBackend>(std::move(backend_config));

    auto created = rt::Context::create(std::move(options));
    REQUIRE(created.has_value());

    rt::InspectionRequest request{};
    request.recipe_id = "tmpl";
    request.request_id = "developer";
    request.timeout_ms = 5000u;
    auto outcome = created.value()->inspect(request);
    REQUIRE(outcome.has_value());
    CHECK(outcome.value().status == core::Status::ok);
    CHECK(outcome.value().verdict == core::Verdict::pass);
    CHECK(outcome.value().output_json.at("x").get<int>() == 21);
    CHECK(outcome.value().output_json.at("y").get<int>() == 17);
}

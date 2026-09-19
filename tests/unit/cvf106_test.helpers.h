#pragma once

// CVF-106 independent black-box test support (owner: test-engineer).
//
// Shared aliases, deterministic asymmetric template/frame builders, run-time
// PNG assets, recipe/config writers with an optional "assets" mapping, direct
// prepared-algorithm helpers, and the runtime lifecycle harness used by the
// independent prepared-asset / template.match suite.
//
// This header compiles only against the frozen interface headers named in the
// CVF-106 test brief; it never includes production .cpp files and never inspects
// private state. Interface spellings beyond the brief text are recorded as
// author assumptions in .ai/reports/CVF-106-test-red.yaml.
//
// Determinism / golden policy
// ---------------------------
// The template fixture is generated at run time (no committed binary image) and
// is intentionally asymmetric in both axes so a transposed or vertically
// mirrored match cannot score as high. Because the frame contains an exact copy
// of the template, ccoeff_normed is expected to be 1.0; all score comparisons
// use a 1e-6 absolute tolerance (Catch::Approx) and coordinates are exact
// integers. No wall-clock sleeps and no machine-specific paths are used.

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

#include "camera/captured_frame.h"

#include "inspection/algorithm.h"
#include "inspection/registry.h"

#include "algorithms/compiled_algorithms.h"
#include "algorithms/template_match.h"

#include "runtime/context.h"

#include "camera/test_backends/file_camera_backend.h"
#include "camera/test_backends/synthetic_camera_backend.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cvf106 {

namespace core = cvforwin::core;
namespace insp = cvforwin::inspection;
namespace alg = cvforwin::algorithms;
namespace cam = cvforwin::camera;
namespace rt = cvforwin::runtime;

using Json = nlohmann::json;

inline constexpr std::string_view k_template_key = "template.match";
inline constexpr std::string_view k_example_key = "example.threshold";
inline constexpr std::string_view k_asset_logical_key = "tmpl";
inline constexpr std::string_view k_asset_reference = "tmpl.png";

// Encoded-asset bound from inspection_recipe_v1.asset_rules.
inline constexpr std::size_t k_max_asset_bytes = 16777216u;

// --- run-time temporary directories ----------------------------------------

class TempDir {
public:
    explicit TempDir(std::string_view tag)
    {
        namespace fs = std::filesystem;
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        path_ = fs::temp_directory_path() / ("cvf106_" + std::string(tag) + "_" + unique);
        std::error_code error;
        fs::remove_all(path_, error);
        if (!fs::create_directories(path_, error)) {
            throw std::runtime_error("cvf106: cannot create temp directory " + path_.string());
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

inline void write_text(const std::filesystem::path& file, std::string_view text)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

inline void write_json(const std::filesystem::path& file, const Json& value)
{
    write_text(file, value.dump(2));
}

// --- deterministic asymmetric fixtures --------------------------------------

// 5x4 single-channel template with strictly increasing, asymmetric structure in
// both axes. Generated deterministically; no image file is committed.
inline cv::Mat asymmetric_template()
{
    static const unsigned char k_pixels[4][5] = {
        {10, 40, 90, 160, 230},
        {200, 20, 70, 140, 210},
        {60, 130, 250, 30, 100},
        {220, 110, 15, 180, 55},
    };
    cv::Mat tmpl(4, 5, CV_8UC1);
    for (int y = 0; y < tmpl.rows; ++y) {
        for (int x = 0; x < tmpl.cols; ++x) {
            tmpl.at<unsigned char>(y, x) = k_pixels[y][x];
        }
    }
    return tmpl;
}

// Uniform (zero-variance) single-channel template of the requested size.
inline cv::Mat uniform_template(int width, int height, unsigned char value)
{
    return cv::Mat(height, width, CV_8UC1, cv::Scalar(value));
}

// A differently-asymmetric template used to prove reload changes behavior.
inline cv::Mat asymmetric_template_alt()
{
    cv::Mat tmpl(4, 5, CV_8UC1);
    for (int y = 0; y < tmpl.rows; ++y) {
        for (int x = 0; x < tmpl.cols; ++x) {
            const int value = (x * 53 + y * 17 + x * y * 29 + 3) % 251;
            tmpl.at<unsigned char>(y, x) = static_cast<unsigned char>(value);
        }
    }
    tmpl.at<unsigned char>(0, 0) = 1;
    return tmpl;
}

// BGR8 frame with a deterministic non-constant background and the single-channel
// template copied into all three channels at (origin_x, origin_y).
inline cv::Mat frame_with_template(int width, int height, const cv::Mat& tmpl, int origin_x,
                                   int origin_y)
{
    REQUIRE(tmpl.type() == CV_8UC1);
    cv::Mat frame(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int value = (x * 7 + y * 13 + 21) % 256;
            frame.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<unsigned char>(value),
                                                  static_cast<unsigned char>((value + 40) % 256),
                                                  static_cast<unsigned char>((value + 90) % 256));
        }
    }
    const cv::Rect region(origin_x, origin_y, tmpl.cols, tmpl.rows);
    REQUIRE(region.x >= 0);
    REQUIRE(region.y >= 0);
    REQUIRE(region.x + region.width <= width);
    REQUIRE(region.y + region.height <= height);
    cv::Mat destination = frame(region);
    cv::Mat channels[3];
    cv::split(destination, channels);
    tmpl.copyTo(channels[0]);
    tmpl.copyTo(channels[1]);
    tmpl.copyTo(channels[2]);
    cv::merge(channels, 3, destination);
    return frame;
}

// BGR8 frame that does not contain the template (deterministic gradient only).
inline cv::Mat frame_without_template(int width, int height)
{
    return frame_with_template(width, height, uniform_template(1, 1, 0), 0, 0);
}

inline cam::CapturedFrame captured_frame(cv::Mat pixels)
{
    cam::CapturedFrame frame{};
    frame.pixels = std::move(pixels);
    frame.metadata.width = static_cast<std::uint32_t>(frame.pixels.cols);
    frame.metadata.height = static_cast<std::uint32_t>(frame.pixels.rows);
    frame.metadata.pixel_format = cam::PixelFormat::bgr8;
    return frame;
}

// --- PNG assets -------------------------------------------------------------

inline std::vector<std::uint8_t> encode_png(const cv::Mat& image)
{
    std::vector<unsigned char> buffer;
    REQUIRE(cv::imencode(".png", image, buffer));
    return std::vector<std::uint8_t>(buffer.begin(), buffer.end());
}

inline void write_png(const std::filesystem::path& file, const cv::Mat& image)
{
    std::filesystem::create_directories(file.parent_path());
    REQUIRE(cv::imwrite(file.string(), image));
}

// --- recipe / config documents ----------------------------------------------

inline Json camera_block()
{
    return Json{
        {"backend", "file"},
        {"device_path", ""},
        {"vendor_id", "0000"},
        {"product_id", "0000"},
        {"friendly_name", "cvf106 file camera"},
    };
}

inline Json base_config(int width, int height)
{
    return Json{
        {"schema_version", 1},
        {"camera", camera_block()},
        {"base_capture",
         Json{{"width", width}, {"height", height}, {"frame_rate", 30.0}, {"pixel_format", "bgr8"}}},
        {"logging", Json{{"level", "info"}, {"max_file_bytes", 1048576}, {"max_files", 2}}},
        {"retention", Json{{"max_age_days", 30}, {"max_total_bytes", 1073741824}}},
    };
}

inline Json template_parameters(int roi_x, int roi_y, int roi_w, int roi_h, double threshold)
{
    return Json{
        {"template_asset", std::string(k_asset_logical_key)},
        {"roi", Json::array({roi_x, roi_y, roi_w, roi_h})},
        {"method", "ccoeff_normed"},
        {"threshold", threshold},
    };
}

inline Json template_recipe(const std::string& recipe_id, const Json& parameters, int width,
                            int height, const Json& assets)
{
    Json recipe{
        {"schema_version", 1},
        {"recipe_id", recipe_id},
        {"algorithm", std::string(k_template_key)},
        {"parameters", parameters},
        {"capture",
         Json{{"width", width}, {"height", height}, {"frame_rate", 15.0}, {"pixel_format", "bgr8"}, {"settle_frames", 0}}},
        {"artifacts", Json{{"save_policy", "never"}, {"required", false}}},
    };
    if (!assets.is_null()) {
        recipe["assets"] = assets;
    }
    return recipe;
}

inline Json example_recipe(const std::string& recipe_id, int width, int height)
{
    return Json{
        {"schema_version", 1},
        {"recipe_id", recipe_id},
        {"algorithm", std::string(k_example_key)},
        {"parameters", Json{{"threshold", 128}, {"min_pass_ratio", 0.0}}},
        {"capture",
         Json{{"width", width}, {"height", height}, {"frame_rate", 15.0}, {"pixel_format", "bgr8"}, {"settle_frames", 0}}},
        {"artifacts", Json{{"save_policy", "never"}, {"required", false}}},
    };
}

// Writes <root>/cvforwin.json, <root>/recipes/<id>.json, and every provided
// asset under <root>/assets/<reference>.
inline void write_config_root(const std::filesystem::path& root, const Json& config,
                              const std::string& recipe_id, const Json& recipe,
                              const std::vector<std::pair<std::string, cv::Mat>>& assets = {})
{
    write_json(root / "cvforwin.json", config);
    write_json(root / "recipes" / (recipe_id + ".json"), recipe);
    for (const auto& entry : assets) {
        write_png(root / "assets" / entry.first, entry.second);
    }
}

inline void write_raw_asset(const std::filesystem::path& root, std::string_view reference,
                            std::string_view bytes)
{
    write_text(root / "assets" / std::string(reference), bytes);
}

// --- direct prepared-algorithm seam -----------------------------------------
//
// ASSUMPTION (recorded in the RED report): the additive seam declares
//   struct AlgorithmAsset { std::string key; std::string reference;
//                           std::vector<std::uint8_t> bytes; };
//   struct AlgorithmAssetBundle { std::vector<AlgorithmAsset> assets;
//                                 const AlgorithmAsset* find(std::string_view) const noexcept; };
// and IInspectionAlgorithm::prepare(const nlohmann::json&, const AlgorithmAssetBundle&) const
// returning core::Result<std::unique_ptr<IPreparedAlgorithm>>. Keeping every use
// of that seam in this one header means a spelling correction touches one file.

inline insp::AlgorithmAssetBundle make_bundle(std::string key, std::string reference,
                                              std::vector<std::uint8_t> bytes)
{
    insp::AlgorithmAssetBundle bundle{};
    insp::AlgorithmAsset asset{};
    asset.key = std::move(key);
    asset.reference = std::move(reference);
    asset.bytes = std::move(bytes);
    bundle.assets.push_back(std::move(asset));
    return bundle;
}

inline insp::AlgorithmAssetBundle png_bundle(const cv::Mat& template_image)
{
    return make_bundle(std::string(k_asset_logical_key), std::string(k_asset_reference),
                       encode_png(template_image));
}

class RegistryHolder {
public:
    RegistryHolder()
    {
        auto registered = alg::register_compiled_algorithms(registry_);
        REQUIRE(registered.has_value());
    }

    RegistryHolder(const RegistryHolder&) = delete;
    RegistryHolder& operator=(const RegistryHolder&) = delete;

    const insp::IInspectionAlgorithm& require(std::string_view key)
    {
        auto found = registry_.find(key);
        REQUIRE(found.has_value());
        return *found.value();
    }

private:
    insp::AlgorithmRegistry registry_{};
};

inline core::Result<std::unique_ptr<insp::IPreparedAlgorithm>>
prepare_with(RegistryHolder& registry, std::string_view key, const Json& parameters,
             const insp::AlgorithmAssetBundle& assets)
{
    const insp::IInspectionAlgorithm& algorithm = registry.require(key);
    return algorithm.prepare(parameters, assets);
}

// --- request holder ---------------------------------------------------------

class RequestHolder {
public:
    RequestHolder(cam::CapturedFrame frame, Json parameters, core::Deadline deadline)
        : frame_(std::move(frame)), parameters_(std::move(parameters)), deadline_(deadline)
    {}

    RequestHolder& with_input(Json input_json)
    {
        input_json_ = std::move(input_json);
        return *this;
    }

    insp::AlgorithmRequest build() const
    {
        return insp::AlgorithmRequest{
            .frame = frame_,
            .parameters = parameters_,
            .input_json = input_json_,
            .deadline = deadline_,
        };
    }

private:
    cam::CapturedFrame frame_;
    Json parameters_;
    std::optional<Json> input_json_ = std::nullopt;
    core::Deadline deadline_ = core::Deadline::from_timeout_ms(5000);
};

inline core::Result<insp::AlgorithmResult> dispatch_prepared(const insp::IPreparedAlgorithm& prepared,
                                                             const insp::AlgorithmRequest& request)
{
    return insp::dispatch(prepared, request);
}

// --- result accessors -------------------------------------------------------

inline double score_of(const insp::AlgorithmResult& result)
{
    REQUIRE(result.measurements.is_object());
    REQUIRE(result.measurements.contains("score"));
    return result.measurements.at("score").get<double>();
}

inline int int_measurement(const insp::AlgorithmResult& result, const char* key)
{
    REQUIRE(result.measurements.is_object());
    REQUIRE(result.measurements.contains(key));
    return result.measurements.at(key).get<int>();
}

inline bool has_template_mismatch(const insp::AlgorithmResult& result)
{
    for (const Json& defect : result.defects) {
        if (defect.is_object() && defect.contains("kind") && defect.at("kind").is_string() &&
            defect.at("kind").get<std::string>() == "template_mismatch") {
            return true;
        }
    }
    return false;
}

inline void check_failure(const core::Failure& failure, core::Status status, core::ErrorCode code)
{
    CHECK(failure.status == status);
    CHECK(failure.code == code);
}

template <typename T>
inline void check_failure(const core::Result<T>& result, core::Status status, core::ErrorCode code)
{
    REQUIRE_FALSE(result.has_value());
    check_failure(result.failure(), status, code);
}

// --- runtime lifecycle harness ----------------------------------------------

inline rt::RuntimeOptions runtime_options(const std::filesystem::path& config_root,
                                          const std::filesystem::path& output_root)
{
    rt::RuntimeOptions options{};
    options.config_root = config_root;
    options.output_root = output_root;
    options.enable_file_logging = false;
    options.enable_callback_logging = false;
    return options;
}

inline void set_file_camera(rt::RuntimeOptions& options, const std::filesystem::path& frame_file,
                            int frame_count)
{
    cam::FileCameraConfig config{};
    config.descriptor.backend_key = "file";
    config.descriptor.device_path = frame_file.parent_path().string();
    config.descriptor.friendly_name = "cvf106 file camera";
    for (int index = 0; index < frame_count; ++index) {
        config.frame_paths.push_back(frame_file.string());
    }
    options.camera_override =
        std::make_shared<cam::FileCameraBackend>(std::move(config));
}

inline void set_synthetic_camera(rt::RuntimeOptions& options, int width, int height)
{
    cam::SyntheticCameraConfig config{};
    config.descriptor.backend_key = "synthetic";
    config.descriptor.friendly_name = "";
    config.width = static_cast<std::uint32_t>(width);
    config.height = static_cast<std::uint32_t>(height);
    options.camera_override = std::make_shared<cam::SyntheticCameraBackend>(std::move(config));
}

inline std::unique_ptr<rt::Context> create_context(const rt::RuntimeOptions& options)
{
    auto created = rt::Context::create(options);
    REQUIRE(created.has_value());
    return std::move(created.value());
}

inline std::unique_ptr<rt::Context> try_create_context(const rt::RuntimeOptions& options)
{
    auto created = rt::Context::create(options);
    if (!created.has_value()) {
        return nullptr;
    }
    return std::move(created.value());
}

inline rt::InspectionRequest inspection_request(std::string recipe_id, std::uint32_t timeout_ms)
{
    rt::InspectionRequest request{};
    request.recipe_id = std::move(recipe_id);
    request.request_id = "cvf106";
    request.timeout_ms = timeout_ms;
    return request;
}

// Reads the runtime final result object from an outcome regardless of whether
// the frozen output_json is a JSON value or a serialized string.
inline Json outcome_json(const rt::InspectionOutcome& outcome)
{
    if constexpr (requires { outcome.output_json.is_object(); }) {
        return outcome.output_json;
    } else {
        return Json::parse(std::string(outcome.output_json));
    }
}

// final_result_payload_v1: the runtime serializes "one final object assembled
// from measurements and optional defects". The algorithm measurements are
// therefore flattened into the top-level result object (score/x/y/width/height
// are top-level members) alongside an optional "defects" key; there is no nested
// "measurements" wrapper. Return that top-level object so the accessors below
// keep their meaning.
inline Json outcome_measurements(const rt::InspectionOutcome& outcome)
{
    const Json output = outcome_json(outcome);
    REQUIRE(output.is_object());
    return output;
}

inline double outcome_score(const rt::InspectionOutcome& outcome)
{
    const Json measurements = outcome_measurements(outcome);
    REQUIRE(measurements.contains("score"));
    return measurements.at("score").get<double>();
}

inline int outcome_int(const rt::InspectionOutcome& outcome, const char* key)
{
    const Json measurements = outcome_measurements(outcome);
    REQUIRE(measurements.contains(key));
    return measurements.at(key).get<int>();
}

// --- seams that must survive the additive change ----------------------------

// Mirrors CVF-003's pinned StubAlgorithm without editing the hash-pinned file:
// the pre-existing pure virtuals (key/validate_parameters/inspect) must still be
// sufficient to instantiate an IInspectionAlgorithm after prepare() gains a
// default implementation.
class LegacyAlgorithm final : public insp::IInspectionAlgorithm {
public:
    std::string_view key() const noexcept override
    {
        return "cvf106.legacy";
    }

    core::Result<void> validate_parameters(const Json&) const override
    {
        return core::Result<void>{};
    }

    core::Result<insp::AlgorithmResult> inspect(const insp::AlgorithmRequest&) override
    {
        return core::Result<insp::AlgorithmResult>{insp::AlgorithmResult{
            .verdict = core::Verdict::pass,
            .measurements = Json::object(),
            .defects = Json::array(),
            .diagnostics = "cvf106 legacy algorithm",
        }};
    }
};

}  // namespace cvf106

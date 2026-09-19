// CVF-106 independent black-box tests: immutable prepared state and asset decode.
//
// Covers test-brief B4 (decode once at load; inspection performs no file I/O)
// and B7 (a newer prepared template does not mutate an object already prepared)
// through the additive prepared-algorithm seam only. No production .cpp is read
// or included.

#include "cvf106_test.helpers.h"

#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>

namespace {

using namespace cvf106;

constexpr int k_frame_width = 64;
constexpr int k_frame_height = 48;
constexpr double k_passing_threshold = 0.8;

Json full_roi_parameters(double threshold)
{
    return template_parameters(0, 0, k_frame_width, k_frame_height, threshold);
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    REQUIRE(stream.good());
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                     std::istreambuf_iterator<char>());
}

insp::AlgorithmResult require_ok(
    const core::Result<std::unique_ptr<insp::IPreparedAlgorithm>>& prepared, RequestHolder& request)
{
    REQUIRE(prepared.has_value());
    auto result = dispatch_prepared(*prepared.value(), request.build());
    REQUIRE(result.has_value());
    return result.value();
}

}  // namespace

TEST_CASE("CVF-106 B4 template is decoded at preparation and not re-read during inspect",
          "[cvf106]")
{
    TempDir workspace("decode_once");
    const std::filesystem::path asset_file = workspace.path() / "tmpl.png";
    write_png(asset_file, asymmetric_template());

    // Load the bytes once, exactly as the catalog would, then remove the source
    // file: preparation must succeed from the in-memory bundle and inspection
    // must not touch the filesystem.
    const std::vector<std::uint8_t> bytes = read_bytes(asset_file);
    REQUIRE(bytes.size() > 8u);
    std::error_code error;
    std::filesystem::remove(asset_file, error);
    REQUIRE_FALSE(std::filesystem::exists(asset_file));

    RegistryHolder registry;
    const Json parameters = full_roi_parameters(k_passing_threshold);
    insp::AlgorithmAssetBundle bundle =
        make_bundle(std::string(k_asset_logical_key), std::string(k_asset_reference), bytes);
    auto prepared = prepare_with(registry, k_template_key, parameters, bundle);
    REQUIRE(prepared.has_value());

    const cv::Mat frame =
        frame_with_template(k_frame_width, k_frame_height, asymmetric_template(), 20, 14);
    RequestHolder request(captured_frame(frame), parameters, core::Deadline::from_timeout_ms(5000));
    const insp::AlgorithmResult first = require_ok(prepared, request);

    // Explicitly re-attempt the original file path to prove nothing recreates it.
    CHECK_FALSE(std::filesystem::exists(asset_file));
    CHECK(first.verdict == core::Verdict::pass);
    CHECK(int_measurement(first, "x") == 20);
    CHECK(int_measurement(first, "y") == 14);
}

TEST_CASE("CVF-106 B7 a newly prepared template does not mutate an already prepared object",
          "[cvf106]")
{
    RegistryHolder registry;
    const Json parameters = full_roi_parameters(k_passing_threshold);

    auto old_prepared =
        prepare_with(registry, k_template_key, parameters, png_bundle(asymmetric_template()));
    REQUIRE(old_prepared.has_value());

    // A "reload" prepares a different immutable object; the old one must keep
    // its prior prepared template.
    const cv::Mat alt = asymmetric_template_alt();
    auto new_prepared = prepare_with(registry, k_template_key, parameters, png_bundle(alt));
    REQUIRE(new_prepared.has_value());

    const cv::Mat old_frame =
        frame_with_template(k_frame_width, k_frame_height, asymmetric_template(), 20, 14);
    const cv::Mat new_frame = frame_with_template(k_frame_width, k_frame_height, alt, 33, 7);

    RequestHolder old_request(captured_frame(old_frame), parameters,
                              core::Deadline::from_timeout_ms(5000));
    RequestHolder new_request(captured_frame(new_frame), parameters,
                              core::Deadline::from_timeout_ms(5000));

    const insp::AlgorithmResult old_result = require_ok(old_prepared, old_request);
    const insp::AlgorithmResult new_result = require_ok(new_prepared, new_request);

    CHECK(old_result.verdict == core::Verdict::pass);
    CHECK(int_measurement(old_result, "x") == 20);
    CHECK(int_measurement(old_result, "y") == 14);
    CHECK(new_result.verdict == core::Verdict::pass);
    CHECK(int_measurement(new_result, "x") == 33);
    CHECK(int_measurement(new_result, "y") == 7);
}

TEST_CASE("CVF-106 B1 preparation rejects an invalid ROI and a larger-than-ROI template",
          "[cvf106]")
{
    RegistryHolder registry;

    SECTION("zero-width ROI")
    {
        auto prepared = prepare_with(registry, k_template_key,
                                     template_parameters(0, 0, 0, 10, k_passing_threshold),
                                     png_bundle(asymmetric_template()));
        REQUIRE_FALSE(prepared.has_value());
        CHECK(prepared.failure().status == core::Status::config_error);
    }

    SECTION("negative ROI origin")
    {
        auto prepared = prepare_with(registry, k_template_key,
                                     template_parameters(-1, 0, k_frame_width, k_frame_height,
                                                         k_passing_threshold),
                                     png_bundle(asymmetric_template()));
        REQUIRE_FALSE(prepared.has_value());
        CHECK(prepared.failure().status == core::Status::config_error);
    }

    SECTION("ROI larger than the configured capture frame is rejected")
    {
        auto prepared = prepare_with(registry, k_template_key,
                                     template_parameters(0, 0, k_frame_width + 1, k_frame_height,
                                                         k_passing_threshold),
                                     png_bundle(asymmetric_template()));
        REQUIRE_FALSE(prepared.has_value());
        CHECK(prepared.failure().status == core::Status::config_error);
    }

    SECTION("template larger than the ROI")
    {
        auto prepared = prepare_with(registry, k_template_key,
                                     template_parameters(0, 0, 2, 2, k_passing_threshold),
                                     png_bundle(asymmetric_template()));
        REQUIRE_FALSE(prepared.has_value());
        CHECK(prepared.failure().status == core::Status::config_error);
    }
}

TEST_CASE("CVF-106 negative: empty asset bytes are rejected at preparation", "[cvf106]")
{
    RegistryHolder registry;
    insp::AlgorithmAssetBundle bundle = make_bundle(std::string(k_asset_logical_key),
                                                    std::string(k_asset_reference), {});
    auto prepared =
        prepare_with(registry, k_template_key, full_roi_parameters(k_passing_threshold), bundle);
    REQUIRE_FALSE(prepared.has_value());
    CHECK(prepared.failure().status == core::Status::config_error);
}

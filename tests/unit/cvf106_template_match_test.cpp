// CVF-106 independent black-box tests: template.match direct prepared behavior.
//
// Covers test-brief B2, B3, B5, B6, the boundary_cases (threshold exactly equal,
// ROI at frame edges, 1x1 rejection, template exactly ROI-sized), the negative
// parameter/asset cases, and the additive-seam compatibility with the pinned
// CVF-003 algorithm shape. No production .cpp is read or included.

#include "cvf106_test.helpers.h"

#include <limits>

namespace {

using namespace cvf106;

constexpr int k_frame_width = 64;
constexpr int k_frame_height = 48;
constexpr int k_template_x = 20;
constexpr int k_template_y = 14;
constexpr double k_passing_threshold = 0.8;

Json full_roi_parameters(double threshold)
{
    return template_parameters(0, 0, k_frame_width, k_frame_height, threshold);
}

// Prepares one template.match instance from the asymmetric template PNG and
// dispatches one frame through it.
core::Result<insp::AlgorithmResult> run_template_match(RegistryHolder& registry,
                                                       const Json& parameters,
                                                       const cam::CapturedFrame& frame,
                                                       core::Deadline deadline)
{
    insp::AlgorithmAssetBundle bundle = png_bundle(asymmetric_template());
    auto prepared = prepare_with(registry, k_template_key, parameters, bundle);
    REQUIRE(prepared.has_value());
    RequestHolder request(frame, parameters, deadline);
    return dispatch_prepared(*prepared.value(), request.build());
}

insp::AlgorithmResult require_ok(const core::Result<insp::AlgorithmResult>& result)
{
    REQUIRE(result.has_value());
    return result.value();
}

}  // namespace

TEST_CASE("CVF-106 B2 template present at a known location passes with global coordinates", "[cvf106]")
{
    RegistryHolder registry;
    const cv::Mat frame =
        frame_with_template(k_frame_width, k_frame_height, asymmetric_template(), k_template_x,
                            k_template_y);

    const core::Result<insp::AlgorithmResult> result =
        run_template_match(registry, full_roi_parameters(k_passing_threshold),
                           captured_frame(frame), core::Deadline::from_timeout_ms(5000));
    const insp::AlgorithmResult matched = require_ok(result);

    CHECK(matched.verdict == core::Verdict::pass);
    const double score = score_of(matched);
    CHECK(score >= k_passing_threshold);
    CHECK(score == Catch::Approx(1.0).margin(1e-6));
    CHECK(int_measurement(matched, "x") == k_template_x);
    CHECK(int_measurement(matched, "y") == k_template_y);
    CHECK(int_measurement(matched, "width") == asymmetric_template().cols);
    CHECK(int_measurement(matched, "height") == asymmetric_template().rows);
    CHECK(matched.defects.empty());
}

TEST_CASE("CVF-106 B3 frame without the template fails with a deterministic defect", "[cvf106]")
{
    RegistryHolder registry;
    const cv::Mat frame = frame_without_template(k_frame_width, k_frame_height);

    const core::Result<insp::AlgorithmResult> result =
        run_template_match(registry, full_roi_parameters(k_passing_threshold),
                           captured_frame(frame), core::Deadline::from_timeout_ms(5000));
    const insp::AlgorithmResult matched = require_ok(result);

    CHECK(matched.verdict == core::Verdict::fail);
    CHECK(score_of(matched) < k_passing_threshold);
    CHECK(has_template_mismatch(matched));
}

TEST_CASE("CVF-106 B5 ROI restricts the search to the requested region", "[cvf106]")
{
    RegistryHolder registry;
    // The template is present at (40, 30) but the ROI only covers the top-left
    // 16x16, so it must not be reported inside the ROI.
    const cv::Mat frame =
        frame_with_template(k_frame_width, k_frame_height, asymmetric_template(), 40, 30);

    const core::Result<insp::AlgorithmResult> result =
        run_template_match(registry, template_parameters(0, 0, 16, 16, k_passing_threshold),
                           captured_frame(frame), core::Deadline::from_timeout_ms(5000));
    const insp::AlgorithmResult matched = require_ok(result);

    CHECK(matched.verdict == core::Verdict::fail);
    CHECK(score_of(matched) < k_passing_threshold);
    CHECK(has_template_mismatch(matched));
}

TEST_CASE("CVF-106 B6 an already-expired deadline fails with algorithm_deadline_exceeded",
          "[cvf106]")
{
    RegistryHolder registry;
    const cv::Mat frame =
        frame_with_template(k_frame_width, k_frame_height, asymmetric_template(), k_template_x,
                            k_template_y);

    const core::Result<insp::AlgorithmResult> result =
        run_template_match(registry, full_roi_parameters(k_passing_threshold),
                           captured_frame(frame), core::Deadline::immediate());
    REQUIRE_FALSE(result.has_value());
    CHECK(result.failure().status == core::Status::timeout);
    CHECK(result.failure().code == core::ErrorCode::algorithm_deadline_exceeded);
}

TEST_CASE("CVF-106 boundary: score exactly equal to the threshold passes", "[cvf106]")
{
    RegistryHolder registry;
    const cv::Mat frame =
        frame_with_template(k_frame_width, k_frame_height, asymmetric_template(), k_template_x,
                            k_template_y);

    const insp::AlgorithmResult probe =
        require_ok(run_template_match(registry, full_roi_parameters(0.0), captured_frame(frame),
                                      core::Deadline::from_timeout_ms(5000)));
    const double measured = score_of(probe);

    const insp::AlgorithmResult at_boundary =
        require_ok(run_template_match(registry, full_roi_parameters(measured),
                                      captured_frame(frame),
                                      core::Deadline::from_timeout_ms(5000)));
    CHECK(at_boundary.verdict == core::Verdict::pass);
    CHECK(score_of(at_boundary) == Catch::Approx(measured).margin(1e-9));
}

TEST_CASE("CVF-106 boundary: ROI at the frame edges reports edge coordinates", "[cvf106]")
{
    RegistryHolder registry;
    const cv::Mat tmpl = asymmetric_template();

    const cv::Mat top_left =
        frame_with_template(k_frame_width, k_frame_height, tmpl, 0, 0);
    const insp::AlgorithmResult corner =
        require_ok(run_template_match(registry, full_roi_parameters(k_passing_threshold),
                                      captured_frame(top_left),
                                      core::Deadline::from_timeout_ms(5000)));
    CHECK(corner.verdict == core::Verdict::pass);
    CHECK(int_measurement(corner, "x") == 0);
    CHECK(int_measurement(corner, "y") == 0);

    const int right = k_frame_width - tmpl.cols;
    const int bottom = k_frame_height - tmpl.rows;
    const cv::Mat bottom_right = frame_with_template(k_frame_width, k_frame_height, tmpl, right, bottom);
    const insp::AlgorithmResult far_corner =
        require_ok(run_template_match(registry, full_roi_parameters(k_passing_threshold),
                                      captured_frame(bottom_right),
                                      core::Deadline::from_timeout_ms(5000)));
    CHECK(far_corner.verdict == core::Verdict::pass);
    CHECK(int_measurement(far_corner, "x") == right);
    CHECK(int_measurement(far_corner, "y") == bottom);
}

TEST_CASE("CVF-106 boundary: a template exactly the size of the ROI matches", "[cvf106]")
{
    RegistryHolder registry;
    const cv::Mat tmpl = asymmetric_template();
    const cv::Mat frame =
        frame_with_template(k_frame_width, k_frame_height, tmpl, k_template_x, k_template_y);

    const insp::AlgorithmResult matched =
        require_ok(run_template_match(registry,
                                      template_parameters(k_template_x, k_template_y, tmpl.cols,
                                                          tmpl.rows, k_passing_threshold),
                                      captured_frame(frame), core::Deadline::from_timeout_ms(5000)));
    CHECK(matched.verdict == core::Verdict::pass);
    CHECK(int_measurement(matched, "x") == k_template_x);
    CHECK(int_measurement(matched, "y") == k_template_y);
}

TEST_CASE("CVF-106 negative: 1x1 template and 1x1 ROI are rejected", "[cvf106]")
{
    RegistryHolder registry;

    SECTION("1x1 zero-size-relative template")
    {
        insp::AlgorithmAssetBundle bundle =
            png_bundle(uniform_template(1, 1, 128));
        auto prepared =
            prepare_with(registry, k_template_key, template_parameters(0, 0, 1, 1, 0.5), bundle);
        REQUIRE_FALSE(prepared.has_value());
        CHECK(prepared.failure().status == core::Status::config_error);
    }

    SECTION("1x1 ROI is smaller than the template")
    {
        auto prepared = prepare_with(registry, k_template_key,
                                     template_parameters(0, 0, 1, 1, 0.5), png_bundle(asymmetric_template()));
        REQUIRE_FALSE(prepared.has_value());
        CHECK(prepared.failure().status == core::Status::config_error);
    }
}

TEST_CASE("CVF-106 negative: template larger than the ROI is rejected", "[cvf106]")
{
    RegistryHolder registry;
    auto prepared = prepare_with(registry, k_template_key,
                                 template_parameters(0, 0, 3, 3, 0.5),
                                 png_bundle(asymmetric_template()));
    REQUIRE_FALSE(prepared.has_value());
    CHECK(prepared.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 negative: zero-variance template is rejected", "[cvf106]")
{
    RegistryHolder registry;
    auto prepared = prepare_with(registry, k_template_key,
                                 template_parameters(0, 0, k_frame_width, k_frame_height, 0.5),
                                 png_bundle(uniform_template(5, 4, 200)));
    REQUIRE_FALSE(prepared.has_value());
    CHECK(prepared.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 negative: malformed template bytes are rejected", "[cvf106]")
{
    RegistryHolder registry;
    insp::AlgorithmAssetBundle bundle =
        make_bundle(std::string(k_asset_logical_key), std::string(k_asset_reference),
                    std::vector<std::uint8_t>(64, 0xAB));
    auto prepared = prepare_with(registry, k_template_key,
                                 template_parameters(0, 0, k_frame_width, k_frame_height, 0.5),
                                 bundle);
    REQUIRE_FALSE(prepared.has_value());
    CHECK(prepared.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 negative: a missing asset key is rejected", "[cvf106]")
{
    RegistryHolder registry;
    insp::AlgorithmAssetBundle bundle =
        make_bundle("other", std::string(k_asset_reference), encode_png(asymmetric_template()));
    auto prepared = prepare_with(registry, k_template_key,
                                 template_parameters(0, 0, k_frame_width, k_frame_height, 0.5),
                                 bundle);
    REQUIRE_FALSE(prepared.has_value());
    CHECK(prepared.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 negative: unknown parameter key is rejected", "[cvf106]")
{
    RegistryHolder registry;
    Json parameters = full_roi_parameters(k_passing_threshold);
    parameters["unexpected"] = 1;
    auto prepared = prepare_with(registry, k_template_key, parameters,
                                 png_bundle(asymmetric_template()));
    REQUIRE_FALSE(prepared.has_value());
    CHECK(prepared.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 negative: non-finite threshold is rejected", "[cvf106]")
{
    RegistryHolder registry;
    Json parameters = full_roi_parameters(k_passing_threshold);
    parameters["threshold"] = std::numeric_limits<double>::quiet_NaN();
    auto prepared = prepare_with(registry, k_template_key, parameters,
                                 png_bundle(asymmetric_template()));
    REQUIRE_FALSE(prepared.has_value());
    CHECK(prepared.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 additive seam: the pinned pre-prepare algorithm shape still dispatches",
          "[cvf106]")
{
    insp::AlgorithmRegistry registry;
    auto added = registry.add(std::make_unique<LegacyAlgorithm>());
    REQUIRE(added.has_value());
    auto found = registry.find("cvf106.legacy");
    REQUIRE(found.has_value());

    RequestHolder request(captured_frame(frame_without_template(8, 8)), Json::object(),
                          core::Deadline::from_timeout_ms(5000));
    const core::Result<insp::AlgorithmResult> result =
        insp::dispatch(*found.value(), request.build());
    const insp::AlgorithmResult dispatched = require_ok(result);
    CHECK(dispatched.verdict == core::Verdict::pass);
}

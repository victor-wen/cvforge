// CVF-006 independent black-box tests: per-recipe capture save policy and
// optional-versus-required persistence failures (brief B9).
#include <filesystem>

#include "cvf006_test_support.h"

using namespace cvf006;

namespace {

void check_saved_frame(const rt::InspectionOutcome& outcome,
                       const std::shared_ptr<cam::SyntheticCameraBackend>& backend)
{
    REQUIRE_FALSE(outcome.image_path.empty());

    const std::filesystem::path file = image_file_of(outcome);
    CHECK(file.is_absolute());
    REQUIRE(std::filesystem::exists(file));
    CHECK(std::filesystem::is_regular_file(file));

    const cv::Mat decoded = cv::imread(file.string(), cv::IMREAD_UNCHANGED);
    REQUIRE_FALSE(decoded.empty());
    CHECK(decoded.cols == kFrameWidth);
    CHECK(decoded.rows == kFrameHeight);
    CHECK(decoded.channels() == 3);

    // The saved image must be the frame the algorithm evaluated: the last frame
    // the backend captured (settle_frames is 0 in the fixtures).
    REQUIRE(backend->capture_call_count >= 1);
    const auto sequence = static_cast<std::uint64_t>(backend->capture_call_count - 1);
    CHECK(decoded.at<cv::Vec3b>(0, 0) == synthetic_pixel(sequence, 0, 0));
    CHECK(decoded.at<cv::Vec3b>(kFrameHeight - 1, kFrameWidth - 1) ==
          synthetic_pixel(sequence, kFrameWidth - 1, kFrameHeight - 1));
}

}  // namespace

TEST_CASE("CVF-006 B9: fail_or_error saves the frame on FAIL and the file decodes as that frame",
          "[cvf-006][B9]")
{
    SyntheticHarness harness("b9_fail_or_error_fail", "example.fail", fail_parameters(),
                             "fail_or_error", false);

    const RequestHolder request("example.fail", "b9-save-on-fail", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    REQUIRE(outcome.status == core::Status::ok);
    REQUIRE(outcome.verdict == core::Verdict::fail);
    check_saved_frame(outcome, harness.backend());
    CHECK(harness.backend()->capture_call_count == 1);
}

TEST_CASE("CVF-006 B9: fail_or_error skips persistence on PASS", "[cvf-006][B9]")
{
    SyntheticHarness harness("b9_fail_or_error_pass", "example.pass", pass_parameters(),
                             "fail_or_error", false);

    const RequestHolder request("example.pass", "b9-skip-on-pass", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    REQUIRE(outcome.status == core::Status::ok);
    REQUIRE(outcome.verdict == core::Verdict::pass);
    CHECK(outcome.image_path.empty());
}

TEST_CASE("CVF-006 B9: always saves on PASS", "[cvf-006][B9]")
{
    SyntheticHarness harness("b9_always_pass", "example.pass", pass_parameters(), "always", false);

    const RequestHolder request("example.pass", "b9-always-pass", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    REQUIRE(outcome.status == core::Status::ok);
    REQUIRE(outcome.verdict == core::Verdict::pass);
    check_saved_frame(outcome, harness.backend());
}

TEST_CASE("CVF-006 B9: never never persists, even on FAIL", "[cvf-006][B9]")
{
    SyntheticHarness harness("b9_never_fail", "example.fail", fail_parameters(), "never", false);

    const RequestHolder request("example.fail", "b9-never-fail", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    REQUIRE(outcome.status == core::Status::ok);
    REQUIRE(outcome.verdict == core::Verdict::fail);
    CHECK(outcome.image_path.empty());
}

TEST_CASE("CVF-006 B9: an optional save failure keeps OK and the verdict and sets warning bit 1",
          "[cvf-006][B9][warnings]")
{
    SyntheticHarness harness("b9_optional_failure", "example.pass", pass_parameters(), "always",
                             false);
    // Invalidate the output location after a successful initialization so the
    // save itself must fail while the already computed verdict stays valid.
    replace_with_blocking_file(harness.output_root(), "cvf006 optional save blocker");

    const RequestHolder request("example.pass", "b9-optional-failure", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::pass);
    CHECK(outcome.image_path.empty());
    CHECK((warning_bits(outcome.warning_flags) & kImageSaveWarningBit) != 0u);
}

TEST_CASE("CVF-006 B9: a required save failure returns REQUIRED_ARTIFACT_ERROR and NOT_EVALUATED",
          "[cvf-006][B9]")
{
    SyntheticHarness harness("b9_required_failure", "example.pass", pass_parameters(), "always",
                             true);
    replace_with_blocking_file(harness.output_root(), "cvf006 required save blocker");

    const RequestHolder request("example.pass", "b9-required-failure", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    CHECK(outcome.status == core::Status::required_artifact_error);
    CHECK(outcome.verdict == core::Verdict::not_evaluated);
    CHECK(outcome.image_path.empty());
}

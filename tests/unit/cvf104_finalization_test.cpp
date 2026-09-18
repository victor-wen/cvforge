/*
 * CVF-104 independent black-box unit tests for the frozen finalization seam
 * src/runtime/finalization.h.
 *
 * Coverage: save_requirement_for mapping, the finalize_after_frame precedence
 * rules (brief B3/B4/B5/B6/B7), the boundary cases (deadline expired at save,
 * save committed just before expiry, no valid frame), the optional-versus-
 * required error precedence, and warn_base pass-through.
 *
 * Only the frozen seam header is included. No production .cpp is read and no
 * implementation detail is assumed beyond the change contract's frozen
 * declaration. The detailed error_code for timeout and required_artifact_error
 * is not pinned by the contract, so those cases assert the contract invariants
 * (nonzero code, status_for(error_code) == status) instead of guessing an
 * enumerator; the post-frame technical-error case asserts the echoed code.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

#include "runtime/finalization.h"

namespace {

namespace core = cvforwin::core;
namespace rt = cvforwin::runtime;

constexpr std::uint32_t kLogBit = 1u << 0u;
constexpr std::uint32_t kImageBit = 1u << 1u;

bool image_warning(std::uint32_t flags) noexcept
{
    return (flags & kImageBit) != 0u;
}

rt::FinalizeRequest base_request()
{
    rt::FinalizeRequest request;
    request.verdict = core::Verdict::pass;
    request.execution_ok = true;
    request.frame_valid = true;
    request.post_frame_status = core::Status::ok;
    request.post_frame_error = core::ErrorCode::none;
    request.requirement = rt::SaveRequirement::none;
    request.save_attempted = false;
    request.save_committed = false;
    request.save_failed = false;
    request.deadline_expired = false;
    return request;
}

void check_failure_code(const rt::FinalizeDecision& decision)
{
    CHECK(decision.error_code != core::ErrorCode::none);
    CHECK(core::status_for(decision.error_code) == decision.status);
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* save_requirement_for mapping.                                              */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 finalization: save_requirement_for maps save policy and required flag",
          "[cvf-104][finalization][requirement]")
{
    using rt::SaveRequirement;

    CHECK(rt::save_requirement_for("never", false) == SaveRequirement::none);
    CHECK(rt::save_requirement_for("never", true) == SaveRequirement::none);
    CHECK(rt::save_requirement_for("always", false) == SaveRequirement::optional);
    CHECK(rt::save_requirement_for("always", true) == SaveRequirement::required);
    CHECK(rt::save_requirement_for("fail_or_error", false) == SaveRequirement::optional);
    CHECK(rt::save_requirement_for("fail_or_error", true) == SaveRequirement::required);

    /* Unknown and empty tokens skip, so they carry no requirement. */
    CHECK(rt::save_requirement_for("bogus", true) == SaveRequirement::none);
    CHECK(rt::save_requirement_for("", false) == SaveRequirement::none);
}

/* ------------------------------------------------------------------------- */
/* B3/B4: a post-frame technical error with a committed diagnostic save.      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B3/B4: post-frame algorithm/serialization error returns its status and "
          "publishes the committed image path",
          "[cvf-104][finalization][B3][B4]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::not_evaluated;
    request.execution_ok = false;
    request.post_frame_status = core::Status::algorithm_error;
    request.post_frame_error = core::ErrorCode::algorithm_exception;
    request.requirement = rt::SaveRequirement::optional;
    request.save_attempted = true;
    request.save_committed = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::algorithm_error);
    CHECK(decision.verdict == core::Verdict::not_evaluated);
    CHECK(decision.error_code == core::ErrorCode::algorithm_exception);
    CHECK(decision.publish_image_path);
    CHECK_FALSE(image_warning(decision.warning_flags));

    SECTION("a serialization failure behaves the same way")
    {
        request.post_frame_status = core::Status::internal_error;
        request.post_frame_error = core::ErrorCode::runtime_result_too_large;

        const rt::FinalizeDecision serialization = rt::finalize_after_frame(request, 0u);

        CHECK(serialization.status == core::Status::internal_error);
        CHECK(serialization.verdict == core::Verdict::not_evaluated);
        CHECK(serialization.error_code == core::ErrorCode::runtime_result_too_large);
        CHECK(serialization.publish_image_path);
    }
}

TEST_CASE("CVF-104 B3/B4 boundary: a post-frame technical error without a committed save publishes "
          "no image path",
          "[cvf-104][finalization][B3][B4][boundary]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::not_evaluated;
    request.execution_ok = false;
    request.post_frame_status = core::Status::algorithm_error;
    request.post_frame_error = core::ErrorCode::algorithm_exception;
    request.requirement = rt::SaveRequirement::optional;
    request.save_attempted = true;
    request.save_committed = false;
    request.save_failed = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::algorithm_error);
    CHECK(decision.verdict == core::Verdict::not_evaluated);
    CHECK_FALSE(decision.publish_image_path);
}

/* ------------------------------------------------------------------------- */
/* B2: a successful verdict with a committed optional save publishes the path. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B2: a committed optional save with a valid PASS/FAIL keeps OK and publishes the "
          "image path",
          "[cvf-104][finalization][B2]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::fail;
    request.execution_ok = true;
    request.requirement = rt::SaveRequirement::optional;
    request.save_attempted = true;
    request.save_committed = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::ok);
    CHECK(decision.verdict == core::Verdict::fail);
    CHECK(decision.error_code == core::ErrorCode::none);
    CHECK(decision.publish_image_path);
    CHECK_FALSE(image_warning(decision.warning_flags));
}

/* ------------------------------------------------------------------------- */
/* B5: an optional save that cannot complete never changes the verdict.       */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B5: an optional non-commit keeps OK and the verdict, sets the image warning, and "
          "publishes nothing",
          "[cvf-104][finalization][B5]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::pass;
    request.execution_ok = true;
    request.requirement = rt::SaveRequirement::optional;
    request.save_attempted = true;
    request.save_committed = false;
    request.save_failed = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::ok);
    CHECK(decision.verdict == core::Verdict::pass);
    CHECK(decision.error_code == core::ErrorCode::none);
    CHECK_FALSE(decision.publish_image_path);
    CHECK(image_warning(decision.warning_flags));

    SECTION("an expired deadline still cannot convert an optional save into a timeout")
    {
        request.deadline_expired = true;

        const rt::FinalizeDecision expired = rt::finalize_after_frame(request, 0u);

        CHECK(expired.status == core::Status::ok);
        CHECK(expired.verdict == core::Verdict::pass);
        CHECK_FALSE(expired.publish_image_path);
        CHECK(image_warning(expired.warning_flags));
    }

    SECTION("an optional save that was never attempted sets no image warning")
    {
        request.save_attempted = false;
        request.save_failed = false;

        const rt::FinalizeDecision skipped = rt::finalize_after_frame(request, 0u);

        CHECK(skipped.status == core::Status::ok);
        CHECK(skipped.verdict == core::Verdict::pass);
        CHECK_FALSE(image_warning(skipped.warning_flags));
    }
}

/* ------------------------------------------------------------------------- */
/* B6: a required save not committed at the absolute deadline times out.      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B6: required save plus expired deadline returns TIMEOUT with NOT_EVALUATED",
          "[cvf-104][finalization][B6]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::pass;
    request.execution_ok = true;
    request.requirement = rt::SaveRequirement::required;
    request.save_attempted = true;
    request.save_committed = false;
    request.save_failed = true;
    request.deadline_expired = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::timeout);
    CHECK(decision.verdict == core::Verdict::not_evaluated);
    check_failure_code(decision);
    CHECK_FALSE(decision.publish_image_path);
    CHECK_FALSE(image_warning(decision.warning_flags));

    SECTION("deadline expiry takes precedence over a required failure")
    {
        const rt::FinalizeDecision timed_out = rt::finalize_after_frame(request, 0u);

        CHECK(timed_out.status == core::Status::timeout);
        CHECK(timed_out.status != core::Status::required_artifact_error);
    }

    SECTION("deadline expiry takes precedence over a required failure even with an earlier "
            "post-frame error")
    {
        request.execution_ok = false;
        request.post_frame_status = core::Status::algorithm_error;
        request.post_frame_error = core::ErrorCode::algorithm_exception;

        const rt::FinalizeDecision timed_out = rt::finalize_after_frame(request, 0u);

        CHECK(timed_out.status == core::Status::timeout);
        CHECK(timed_out.verdict == core::Verdict::not_evaluated);
        CHECK_FALSE(timed_out.publish_image_path);
    }
}

/* ------------------------------------------------------------------------- */
/* B7: a required save failure before the deadline is a required-artifact error. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B7: required save failure before the deadline returns REQUIRED_ARTIFACT_ERROR "
          "with NOT_EVALUATED",
          "[cvf-104][finalization][B7]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::pass;
    request.execution_ok = true;
    request.requirement = rt::SaveRequirement::required;
    request.save_attempted = true;
    request.save_committed = false;
    request.save_failed = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::required_artifact_error);
    CHECK(decision.verdict == core::Verdict::not_evaluated);
    check_failure_code(decision);
    CHECK_FALSE(decision.publish_image_path);
    CHECK_FALSE(image_warning(decision.warning_flags));

    SECTION("a required save that simply did not commit is also a required-artifact error")
    {
        request.save_failed = false;

        const rt::FinalizeDecision not_committed = rt::finalize_after_frame(request, 0u);

        CHECK(not_committed.status == core::Status::required_artifact_error);
        CHECK(not_committed.verdict == core::Verdict::not_evaluated);
    }

    SECTION("required failure takes precedence over an earlier post-frame technical error")
    {
        request.execution_ok = false;
        request.post_frame_status = core::Status::algorithm_error;
        request.post_frame_error = core::ErrorCode::algorithm_exception;

        const rt::FinalizeDecision precedence = rt::finalize_after_frame(request, 0u);

        CHECK(precedence.status == core::Status::required_artifact_error);
        CHECK(precedence.verdict == core::Verdict::not_evaluated);
        CHECK_FALSE(precedence.publish_image_path);
    }
}

/* ------------------------------------------------------------------------- */
/* Boundary: a required save committed just before expiry is a success.       */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 finalization boundary: a required save committed just before expiry keeps the "
          "verdict and publishes the image path",
          "[cvf-104][finalization][boundary][required]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::pass;
    request.requirement = rt::SaveRequirement::required;
    request.save_attempted = true;
    request.save_committed = true;
    request.deadline_expired = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::ok);
    CHECK(decision.verdict == core::Verdict::pass);
    CHECK(decision.error_code == core::ErrorCode::none);
    CHECK(decision.publish_image_path);
}

TEST_CASE("CVF-104 finalization boundary: a required save not committed at the exact expiry times "
          "out",
          "[cvf-104][finalization][boundary][required]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::fail;
    request.requirement = rt::SaveRequirement::required;
    request.save_attempted = true;
    request.save_committed = false;
    request.deadline_expired = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::timeout);
    CHECK(decision.verdict == core::Verdict::not_evaluated);
    CHECK_FALSE(decision.publish_image_path);
}

/* ------------------------------------------------------------------------- */
/* Boundary: no valid frame is ever acquired.                                 */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 finalization boundary: no valid frame makes no artifact attempt and reports the "
          "capture failure",
          "[cvf-104][finalization][boundary][no-frame]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::not_evaluated;
    request.execution_ok = false;
    request.frame_valid = false;
    request.post_frame_status = core::Status::camera_io;
    request.post_frame_error = core::ErrorCode::capture_failed;
    request.requirement = rt::SaveRequirement::optional;
    request.save_attempted = false;
    request.save_committed = false;
    request.save_failed = false;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::camera_io);
    CHECK(decision.verdict == core::Verdict::not_evaluated);
    CHECK(decision.error_code == core::ErrorCode::capture_failed);
    CHECK_FALSE(decision.publish_image_path);
    CHECK_FALSE(image_warning(decision.warning_flags));

    SECTION("no-frame with a required policy still never becomes a required-artifact error")
    {
        request.requirement = rt::SaveRequirement::required;

        const rt::FinalizeDecision required = rt::finalize_after_frame(request, 0u);

        CHECK(required.status == core::Status::camera_io);
        CHECK(required.status != core::Status::required_artifact_error);
        CHECK_FALSE(required.publish_image_path);
    }
}

/* ------------------------------------------------------------------------- */
/* warn_base pass-through.                                                    */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 finalization: warn_base is OR-ed into every decision and never changes status",
          "[cvf-104][finalization][warnings]")
{
    SECTION("successful optional commit")
    {
        rt::FinalizeRequest request = base_request();
        request.requirement = rt::SaveRequirement::optional;
        request.save_attempted = true;
        request.save_committed = true;

        const rt::FinalizeDecision decision = rt::finalize_after_frame(request, kLogBit);

        CHECK(decision.status == core::Status::ok);
        CHECK((decision.warning_flags & kLogBit) != 0u);
        CHECK_FALSE(image_warning(decision.warning_flags));
    }

    SECTION("optional failure OR-s the injected log warning with the image warning")
    {
        rt::FinalizeRequest request = base_request();
        request.requirement = rt::SaveRequirement::optional;
        request.save_attempted = true;
        request.save_committed = false;
        request.save_failed = true;

        const rt::FinalizeDecision decision = rt::finalize_after_frame(request, kLogBit);

        CHECK(decision.status == core::Status::ok);
        CHECK((decision.warning_flags & kLogBit) != 0u);
        CHECK(image_warning(decision.warning_flags));
        CHECK(decision.warning_flags == (kLogBit | kImageBit));
    }

    SECTION("required timeout preserves the injected log warning")
    {
        rt::FinalizeRequest request = base_request();
        request.requirement = rt::SaveRequirement::required;
        request.save_attempted = true;
        request.save_committed = false;
        request.deadline_expired = true;

        const rt::FinalizeDecision decision = rt::finalize_after_frame(request, kLogBit);

        CHECK(decision.status == core::Status::timeout);
        CHECK((decision.warning_flags & kLogBit) != 0u);
    }
}

/* ------------------------------------------------------------------------- */
/* Verify-phase additions (2026-09-19, owner: test-engineer).                 */
/* Requirement-derived edge cases from the CVF-104 brief and the               */
/* artifact_finalization_v1 precedence rules. No existing assertion changed.  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 finalization precedence: a required save that failed at an expired deadline is "
          "TIMEOUT, not REQUIRED_ARTIFACT_ERROR",
          "[cvf-104][finalization][precedence][timeout][boundary]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::pass;
    request.execution_ok = true;
    request.frame_valid = true;
    request.requirement = rt::SaveRequirement::required;
    request.save_attempted = true;
    request.save_committed = false;
    request.save_failed = true;
    request.deadline_expired = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, kLogBit);

    /* The absolute deadline wins even though the required save also failed. */
    CHECK(decision.status == core::Status::timeout);
    CHECK(decision.status != core::Status::required_artifact_error);
    CHECK(decision.verdict == core::Verdict::not_evaluated);
    check_failure_code(decision);
    CHECK_FALSE(decision.publish_image_path);
    /* A required failure never claims the optional image-save warning bit, but a
     * pre-existing warning is preserved. */
    CHECK_FALSE(image_warning(decision.warning_flags));
    CHECK((decision.warning_flags & kLogBit) != 0u);
}

TEST_CASE("CVF-104 finalization boundary: a committed required save at the expiry boundary succeeds",
          "[cvf-104][finalization][boundary][required]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::fail;
    request.execution_ok = true;
    request.frame_valid = true;
    request.requirement = rt::SaveRequirement::required;
    request.save_attempted = true;
    request.save_committed = true;
    request.save_failed = false;
    request.deadline_expired = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, kLogBit);

    CHECK(decision.status == core::Status::ok);
    CHECK(decision.verdict == core::Verdict::fail);
    CHECK(decision.error_code == core::ErrorCode::none);
    CHECK(decision.publish_image_path);
    CHECK_FALSE(image_warning(decision.warning_flags));
    CHECK((decision.warning_flags & kLogBit) != 0u);
}

TEST_CASE("CVF-104 finalization boundary: a committed optional save at the expiry boundary publishes "
          "and keeps the verdict",
          "[cvf-104][finalization][boundary][optional]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::fail;
    request.execution_ok = true;
    request.frame_valid = true;
    request.requirement = rt::SaveRequirement::optional;
    request.save_attempted = true;
    request.save_committed = true;
    request.save_failed = false;
    request.deadline_expired = true;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::ok);
    CHECK(decision.verdict == core::Verdict::fail);
    CHECK(decision.error_code == core::ErrorCode::none);
    CHECK(decision.publish_image_path);
    CHECK_FALSE(image_warning(decision.warning_flags));
}

TEST_CASE("CVF-104 finalization boundary: no frame_valid makes no save attempt and no image warning "
          "even for an optional policy",
          "[cvf-104][finalization][boundary][no-frame][optional]")
{
    rt::FinalizeRequest request = base_request();
    request.verdict = core::Verdict::not_evaluated;
    request.execution_ok = false;
    request.frame_valid = false;
    request.post_frame_status = core::Status::algorithm_error;
    request.post_frame_error = core::ErrorCode::algorithm_exception;
    request.requirement = rt::SaveRequirement::optional;
    request.save_attempted = false;
    request.save_committed = false;
    request.save_failed = false;

    const rt::FinalizeDecision decision = rt::finalize_after_frame(request, 0u);

    CHECK(decision.status == core::Status::algorithm_error);
    CHECK(decision.verdict == core::Verdict::not_evaluated);
    CHECK(decision.error_code == core::ErrorCode::algorithm_exception);
    CHECK_FALSE(decision.publish_image_path);
    CHECK_FALSE(image_warning(decision.warning_flags));

    SECTION("the same engine failure with a required policy still reports the engine error")
    {
        request.requirement = rt::SaveRequirement::required;

        const rt::FinalizeDecision required = rt::finalize_after_frame(request, 0u);

        CHECK(required.status == core::Status::algorithm_error);
        CHECK(required.status != core::Status::required_artifact_error);
        CHECK(required.error_code == core::ErrorCode::algorithm_exception);
        CHECK_FALSE(required.publish_image_path);
        CHECK_FALSE(image_warning(required.warning_flags));
    }
}

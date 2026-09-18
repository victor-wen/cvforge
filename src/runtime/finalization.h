/*
 * Single post-frame finalization decision.
 *
 * Every inspection outcome that follows a captured frame -- success, algorithm
 * failure, serialization failure, or any later technical failure -- passes
 * through finalize_after_frame() exactly once. The decision applies the recipe
 * artifact policy with the real execution_ok value and enforces the contract's
 * deterministic status/error precedence. save_requirement_for() maps a recipe
 * save policy plus its required flag onto the persistence requirement.
 *
 * This header is pure and portable: it performs no I/O, owns no thread, and
 * touches only the core value model, so the finalization seam can be exercised
 * in isolation.
 */

#ifndef CVFORWIN_SRC_RUNTIME_FINALIZATION_H_
#define CVFORWIN_SRC_RUNTIME_FINALIZATION_H_

#include <cstdint>
#include <string_view>

#include "core/error.h"
#include "core/status.h"
#include "core/warnings.h"

namespace cvforwin::runtime {

enum class SaveRequirement {
    none,
    optional,
    required,
};

/* Maps a recipe save policy + required flag onto the persistence requirement. */
inline SaveRequirement save_requirement_for(std::string_view save_policy, bool required) noexcept
{
    if (save_policy == "never") {
        return SaveRequirement::none;
    }
    if (save_policy == "always" || save_policy == "fail_or_error") {
        return required ? SaveRequirement::required : SaveRequirement::optional;
    }
    return SaveRequirement::none;
}

/*
 * Immutable inputs to the single post-frame finalization step. post_frame_status/
 * post_frame_error carry the primary technical failure observed after a valid
 * frame (algorithm, serialization, or later); they are ok/none when execution
 * succeeded.
 */
struct FinalizeRequest {
    core::Verdict verdict = core::Verdict::not_evaluated;
    bool execution_ok = true;
    bool frame_valid = false;
    core::Status post_frame_status = core::Status::ok;
    core::ErrorCode post_frame_error = core::ErrorCode::none;
    SaveRequirement requirement = SaveRequirement::none;
    bool save_attempted = false;
    bool save_committed = false; /* atomic commit finished before the deadline */
    bool save_failed = false; /* encode/write/commit failed before the deadline */
    bool deadline_expired = false;
};

struct FinalizeDecision {
    core::Status status = core::Status::ok;
    core::Verdict verdict = core::Verdict::not_evaluated;
    core::ErrorCode error_code = core::ErrorCode::none;
    std::uint32_t warning_flags = 0;
    bool publish_image_path = false;
};

/*
 * One decision for every post-frame outcome. Precedence:
 *  0. No valid frame ever existed: echo the capture-stage failure untouched and
 *     never turn it into an artifact error.
 *  1. Required save not committed and the absolute deadline expired -> timeout.
 *  2. Required save failed (or not committed) before the deadline ->
 *     required_artifact_error.
 *  3. A post-frame technical error -> its status/error with NOT_EVALUATED,
 *     publishing a committed image_path when the save completed.
 *  4. Otherwise OK with the given verdict; an optional save that did not commit
 *     only sets warning_image_save_failed and never changes the verdict.
 * warn_base is OR-ed in (diagnostics/log-sink warnings).
 */
inline FinalizeDecision finalize_after_frame(const FinalizeRequest& request,
                                             std::uint32_t warn_base) noexcept
{
    FinalizeDecision decision;
    decision.warning_flags = warn_base;

    const bool optional_incomplete =
        request.requirement == SaveRequirement::optional && request.save_attempted && !request.save_committed;

    if (!request.frame_valid) {
        decision.verdict = core::Verdict::not_evaluated;
        if (request.execution_ok && request.post_frame_status == core::Status::ok) {
            decision.status = core::Status::ok;
            decision.error_code = core::ErrorCode::none;
        } else {
            decision.status = request.post_frame_status != core::Status::ok ? request.post_frame_status
                                                                            : core::Status::internal_error;
            decision.error_code =
                request.post_frame_error != core::ErrorCode::none ? request.post_frame_error
                                                                  : core::ErrorCode::internal_unexpected;
        }
        if (optional_incomplete) {
            decision.warning_flags |= static_cast<std::uint32_t>(core::warning_image_save_failed);
        }
        return decision;
    }

    const bool required_incomplete =
        request.requirement == SaveRequirement::required && !request.save_committed;

    if (required_incomplete) {
        decision.verdict = core::Verdict::not_evaluated;
        decision.publish_image_path = false;
        if (request.deadline_expired) {
            decision.status = core::Status::timeout;
            decision.error_code = core::ErrorCode::deadline_expired;
        } else {
            decision.status = core::Status::required_artifact_error;
            decision.error_code = core::ErrorCode::runtime_required_artifact_failed;
        }
        return decision;
    }

    if (!request.execution_ok) {
        decision.status = request.post_frame_status != core::Status::ok ? request.post_frame_status
                                                                        : core::Status::internal_error;
        decision.verdict = core::Verdict::not_evaluated;
        decision.error_code = request.post_frame_error != core::ErrorCode::none ? request.post_frame_error
                                                                                : core::ErrorCode::internal_unexpected;
        decision.publish_image_path = request.save_committed;
        if (optional_incomplete) {
            decision.warning_flags |= static_cast<std::uint32_t>(core::warning_image_save_failed);
        }
        return decision;
    }

    decision.status = core::Status::ok;
    decision.verdict = request.verdict;
    decision.error_code = core::ErrorCode::none;
    decision.publish_image_path = request.save_committed;
    if (optional_incomplete) {
        decision.warning_flags |= static_cast<std::uint32_t>(core::warning_image_save_failed);
    }
    return decision;
}

}  // namespace cvforwin::runtime

#endif /* CVFORWIN_SRC_RUNTIME_FINALIZATION_H_ */

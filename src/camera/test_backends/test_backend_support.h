/*
 * Shared fault-injection support for the deterministic test camera backends.
 *
 * The frozen test-backend API spells the counters as callable accessors
 * (capture_call_count()) while the black-box suite observes them as plain
 * members (capture_call_count == 1). CallCounter supports both spellings with
 * the same value, so the frozen semantics hold in either form.
 *
 * Fault plans are keyed by the 1-based capture call number; they are consumed
 * by the call they target and clear_faults() resets every plan and counter.
 */

#ifndef CVFORWIN_SRC_CAMERA_TEST_BACKENDS_TEST_BACKEND_SUPPORT_H_
#define CVFORWIN_SRC_CAMERA_TEST_BACKENDS_TEST_BACKEND_SUPPORT_H_

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "camera/camera_backend.h"
#include "core/deadline.h"
#include "core/error.h"

namespace cvforwin::camera {

enum class FaultKind {
    disconnect,
    capture_error,
};

class CallCounter {
public:
    std::uint32_t value() const noexcept
    {
        return value_;
    }

    /* Frozen spelling: capture_call_count(). */
    std::uint32_t operator()() const noexcept
    {
        return value_;
    }

    /* Black-box spelling: capture_call_count == n. */
    operator std::uint32_t() const noexcept
    {
        return value_;
    }

    void increment() noexcept
    {
        ++value_;
    }

    void reset() noexcept
    {
        value_ = 0;
    }

private:
    std::uint32_t value_ = 0;
};

bool descriptors_equal(const CameraDescriptor& left, const CameraDescriptor& right);

class FaultInjector {
public:
    struct Outcome {
        bool disconnected = false;
        std::optional<core::Failure> failure;
    };

    void inject_failure(std::uint32_t capture_call_1based, FaultKind kind);
    void inject_delay(std::uint32_t capture_call_1based, std::chrono::milliseconds delay);

    /*
     * Applies and consumes the plans targeting capture_call_1based. A delay
     * honors the deadline (never sleeps past it) and an overrun reports
     * timeout/capture_timed_out; a disconnect reports camera_disconnected and
     * asks the backend to stay disconnected; a capture error reports
     * camera_io/capture_failed once.
     */
    Outcome apply(std::uint32_t capture_call_1based, const core::Deadline& deadline);

    void clear() noexcept;

private:
    struct FailurePlan {
        std::uint32_t capture_call = 0;
        FaultKind kind = FaultKind::capture_error;
    };

    struct DelayPlan {
        std::uint32_t capture_call = 0;
        std::chrono::milliseconds delay{0};
    };

    /* One plan per targeted call; a new injection replaces the plan for that call. */
    std::vector<FailurePlan> failure_plans_;
    std::vector<DelayPlan> delay_plans_;
};

}  // namespace cvforwin::camera

#endif /* CVFORWIN_SRC_CAMERA_TEST_BACKENDS_TEST_BACKEND_SUPPORT_H_ */

#include "camera/test_backends/test_backend_support.h"

#include <algorithm>
#include <thread>
#include <utility>

namespace cvforwin::camera {

bool descriptors_equal(const CameraDescriptor& left, const CameraDescriptor& right)
{
    return left.backend_key == right.backend_key && left.device_path == right.device_path && left.vendor_id == right.vendor_id && left.product_id == right.product_id && left.friendly_name == right.friendly_name;
}

void FaultInjector::inject_failure(std::uint32_t capture_call_1based, FaultKind kind)
{
    const auto existing = std::find_if(failure_plans_.begin(), failure_plans_.end(),
                                       [capture_call_1based](const FailurePlan& plan) { return plan.capture_call == capture_call_1based; });
    if (existing != failure_plans_.end()) {
        existing->kind = kind;
        return;
    }
    failure_plans_.push_back(FailurePlan{capture_call_1based, kind});
}

void FaultInjector::inject_delay(std::uint32_t capture_call_1based, std::chrono::milliseconds delay)
{
    const auto existing = std::find_if(delay_plans_.begin(), delay_plans_.end(),
                                       [capture_call_1based](const DelayPlan& plan) { return plan.capture_call == capture_call_1based; });
    if (existing != delay_plans_.end()) {
        existing->delay = delay;
        return;
    }
    delay_plans_.push_back(DelayPlan{capture_call_1based, delay});
}

FaultInjector::Outcome FaultInjector::apply(std::uint32_t capture_call_1based, const core::Deadline& deadline)
{
    Outcome outcome;

    const auto delay = std::find_if(delay_plans_.begin(), delay_plans_.end(),
                                    [capture_call_1based](const DelayPlan& plan) { return plan.capture_call == capture_call_1based; });
    if (delay != delay_plans_.end()) {
        const auto injected = delay->delay;
        delay_plans_.erase(delay);
        if (deadline.expired()) {
            outcome.failure = core::make_failure(core::Status::timeout, core::ErrorCode::capture_timed_out,
                                                 "injected capture delay found an expired deadline");
            return outcome;
        }
        const auto remaining = deadline.remaining();
        if (injected > remaining) {
            std::this_thread::sleep_for(remaining);
            outcome.failure = core::make_failure(core::Status::timeout, core::ErrorCode::capture_timed_out,
                                                 "injected capture delay overran the deadline");
            return outcome;
        }
        std::this_thread::sleep_for(injected);
    }

    const auto failure = std::find_if(failure_plans_.begin(), failure_plans_.end(),
                                      [capture_call_1based](const FailurePlan& plan) { return plan.capture_call == capture_call_1based; });
    if (failure != failure_plans_.end()) {
        const auto kind = failure->kind;
        failure_plans_.erase(failure);
        if (kind == FaultKind::disconnect) {
            outcome.disconnected = true;
            outcome.failure = core::make_failure(core::Status::camera_io, core::ErrorCode::camera_disconnected,
                                                 "injected camera disconnect");
        } else {
            outcome.failure = core::make_failure(core::Status::camera_io, core::ErrorCode::capture_failed,
                                                 "injected capture failure");
        }
    }

    return outcome;
}

void FaultInjector::clear() noexcept
{
    failure_plans_.clear();
    delay_plans_.clear();
}

}  // namespace cvforwin::camera

#include "camera/camera_backend.h"

#include <utility>

namespace cvforwin::camera {

namespace {

bool selector_is_empty(const CameraSelector& selector)
{
    return selector.device_path.empty() && selector.vendor_id.empty() && selector.product_id.empty() && selector.friendly_name.empty();
}

bool selector_matches(const CameraDescriptor& descriptor, const CameraSelector& selector)
{
    return (selector.device_path.empty() || selector.device_path == descriptor.device_path) && (selector.vendor_id.empty() || selector.vendor_id == descriptor.vendor_id) && (selector.product_id.empty() || selector.product_id == descriptor.product_id) && (selector.friendly_name.empty() || selector.friendly_name == descriptor.friendly_name);
}

core::Failure deadline_expired_failure(const char* stage)
{
    return core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                              std::string{"camera capture deadline expired before "} + stage);
}

}  // namespace

core::Result<CameraDescriptor> resolve_identity(const std::vector<CameraDescriptor>& candidates,
                                                const CameraSelector& selector)
{
    if (selector_is_empty(selector)) {
        return core::make_failure(core::Status::invalid_argument, core::ErrorCode::selector_empty,
                                  "camera selector does not specify any identity field");
    }

    const CameraDescriptor* match = nullptr;
    for (const auto& candidate : candidates) {
        if (!selector_matches(candidate, selector)) {
            continue;
        }
        if (match != nullptr) {
            return core::make_failure(core::Status::camera_not_found, core::ErrorCode::camera_identity_ambiguous,
                                      "camera selector matches more than one enumerated device");
        }
        match = &candidate;
    }

    if (match == nullptr) {
        return core::make_failure(core::Status::camera_not_found, core::ErrorCode::camera_not_found,
                                  "camera selector matches no enumerated device");
    }
    return *match;
}

core::Result<CapturedFrame> capture_with_one_retry(ICameraBackend& backend, const CameraSettings& settings,
                                                   const core::Deadline& deadline)
{
    if (deadline.expired()) {
        return deadline_expired_failure("the first attempt");
    }

    auto first = backend.capture(deadline);
    if (first.has_value()) {
        return first;
    }
    if (first.failure().status != core::Status::camera_io) {
        return first;
    }

    if (deadline.expired()) {
        return deadline_expired_failure("reconnect");
    }

    auto reconnected = backend.reconnect(settings, deadline);
    if (!reconnected.has_value()) {
        return core::Result<CapturedFrame>{reconnected.failure()};
    }

    if (deadline.expired()) {
        return deadline_expired_failure("recapture");
    }

    return backend.capture(deadline);
}

}  // namespace cvforwin::camera

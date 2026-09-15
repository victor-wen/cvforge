/*
 * Internal camera backend contract.
 *
 * Every backend (UVC on Windows, file/synthetic for tests, later approved
 * adapters) implements ICameraBackend and exchanges only backend-neutral
 * values. Identity resolution resolves exactly one device or fails; there is
 * never a first-match fallback. capture_with_one_retry performs at most one
 * reconnect and one recapture inside the remaining deadline.
 */

#ifndef CVFORWIN_SRC_CAMERA_CAMERA_BACKEND_H_
#define CVFORWIN_SRC_CAMERA_CAMERA_BACKEND_H_

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "camera/captured_frame.h"
#include "core/deadline.h"
#include "core/result.h"

namespace cvforwin::camera {

struct CameraDescriptor {
    std::string backend_key;
    std::string device_path;
    std::string vendor_id;
    std::string product_id;
    std::string friendly_name;
};

/* Empty-string field means unspecified; matching is exact and case-sensitive. */
struct CameraSelector {
    std::string device_path;
    std::string vendor_id;
    std::string product_id;
    std::string friendly_name;
};

struct CameraSettings {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frame_rate = 0.0;
    PixelFormat preferred_format = PixelFormat::unknown;
};

class ICameraBackend {
public:
    virtual ~ICameraBackend() = default;

    virtual std::string_view backend_key() const noexcept = 0;
    virtual core::Result<std::vector<CameraDescriptor>> enumerate(const core::Deadline& deadline) = 0;
    virtual core::Result<void> open(const CameraDescriptor& descriptor, const CameraSettings& settings,
                                    const core::Deadline& deadline) = 0;
    virtual core::Result<CapturedFrame> capture(const core::Deadline& deadline) = 0;
    virtual core::Result<void> reconnect(const CameraSettings& settings, const core::Deadline& deadline) = 0;
    virtual void close() noexcept = 0;
};

/*
 * Resolves exactly one candidate. An empty selector is rejected, zero matches
 * fail with camera_not_found, and two or more matches fail with
 * camera_identity_ambiguous; only the uniquely matching descriptor is
 * returned.
 */
core::Result<CameraDescriptor> resolve_identity(const std::vector<CameraDescriptor>& candidates,
                                                const CameraSelector& selector);

/*
 * Bounded capture: one capture, and on a camera_io failure at most one
 * reconnect plus one recapture inside the remaining deadline. Non-camera_io
 * failures (for example timeouts) are returned unchanged, and the helper never
 * enumerates or opens a different descriptor.
 */
core::Result<CapturedFrame> capture_with_one_retry(ICameraBackend& backend, const CameraSettings& settings,
                                                   const core::Deadline& deadline);

}  // namespace cvforwin::camera

#endif /* CVFORWIN_SRC_CAMERA_CAMERA_BACKEND_H_ */

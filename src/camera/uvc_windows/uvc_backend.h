/*
 * Windows Media Foundation UVC camera backend (uvc_windows_backend module).
 *
 * UvcCameraBackend is the driverless UVC path on Windows: it enumerates Media
 * Foundation video-capture devices with stable identity attributes (symbolic
 * link device path, VID/PID, friendly name), resolves exactly one configured
 * identity through the shared camera_contract rules, and opens, configures,
 * captures, reconnects, and closes one device. Captured samples are converted
 * to an owned BGR8 cv::Mat; no Media Foundation type leaves this module.
 *
 * The module is Windows-only: the declaration and the implementation are
 * guarded by _WIN32, the sources compile only under if(WIN32), and the Media
 * Foundation system libraries are linked only on Windows (see CMakeLists.txt).
 * No vendor SDK is used. Every operation returns camera_contract values and
 * never lets an exception escape.
 */

#ifndef CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_BACKEND_H_
#define CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_BACKEND_H_

#if defined(_WIN32)

#include <memory>
#include <string_view>
#include <vector>

#include "camera/camera_backend.h"
#include "camera/captured_frame.h"
#include "core/deadline.h"
#include "core/result.h"

namespace cvforwin::camera {

class UvcCameraBackend final : public ICameraBackend {
public:
    UvcCameraBackend();
    ~UvcCameraBackend() override;

    UvcCameraBackend(const UvcCameraBackend&) = delete;
    UvcCameraBackend& operator=(const UvcCameraBackend&) = delete;

    std::string_view backend_key() const noexcept override;

    /*
     * Returns one descriptor per Media Foundation video-capture device. A
     * system without a camera enumerates successfully with zero descriptors;
     * missing or ambiguous identity is reported by open(), never by silently
     * selecting a device.
     */
    core::Result<std::vector<CameraDescriptor>> enumerate(const core::Deadline& deadline) override;

    /*
     * Re-enumerates, resolves the descriptor to exactly one attached device,
     * activates the Media Foundation source, and selects the closest supported
     * capture format for settings (width, height, frame rate, pixel-format
     * preference). Failures: camera_not_found when the identity does not
     * resolve, camera_io for activation/configuration failures, timeout when
     * the deadline expires.
     */
    core::Result<void> open(const CameraDescriptor& descriptor, const CameraSettings& settings,
                            const core::Deadline& deadline) override;

    /*
     * Reads one sample and returns an owned BGR8 frame normalized to the
     * configured dimensions. An already-expired deadline fails immediately
     * with timeout/capture_timed_out; a device or stream failure fails with
     * camera_io.
     */
    core::Result<CapturedFrame> capture(const core::Deadline& deadline) override;

    /*
     * Releases the current device and re-activates the identity of the last
     * successful open. A device that has not reappeared fails with
     * camera_io/camera_disconnected so that capture_with_one_retry stays
     * bounded and reports camera_io.
     */
    core::Result<void> reconnect(const CameraSettings& settings, const core::Deadline& deadline) override;

    /* Deterministic, idempotent teardown; safe on a never-opened backend. */
    void close() noexcept override;

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

}  // namespace cvforwin::camera

#endif /* defined(_WIN32) */

#endif /* CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_BACKEND_H_ */

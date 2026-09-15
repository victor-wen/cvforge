/*
 * Deterministic synthetic camera backend for hardware-free tests and CI.
 *
 * Frames are generated from the configured (or overridden) size with a fixed
 * per-pixel formula, so a test can recompute every pixel from the frame
 * sequence. The backend also exposes the shared fault injection API
 * (disconnect, capture error, bounded delay) and call counters.
 */

#ifndef CVFORWIN_SRC_CAMERA_TEST_BACKENDS_SYNTHETIC_CAMERA_BACKEND_H_
#define CVFORWIN_SRC_CAMERA_TEST_BACKENDS_SYNTHETIC_CAMERA_BACKEND_H_

#include <chrono>
#include <cstdint>
#include <string_view>
#include <vector>

#include "camera/camera_backend.h"
#include "camera/test_backends/test_backend_support.h"

namespace cvforwin::camera {

struct SyntheticCameraConfig {
    /* The default descriptor identifies the synthetic backend by that key. */
    CameraDescriptor descriptor{.backend_key = "synthetic", .device_path = "", .vendor_id = "", .product_id = "", .friendly_name = ""};
    std::uint32_t width = 64;
    std::uint32_t height = 48;
};

class SyntheticCameraBackend final : public ICameraBackend {
public:
    explicit SyntheticCameraBackend(SyntheticCameraConfig config);

    std::string_view backend_key() const noexcept override;
    core::Result<std::vector<CameraDescriptor>> enumerate(const core::Deadline& deadline) override;
    core::Result<void> open(const CameraDescriptor& descriptor, const CameraSettings& settings,
                            const core::Deadline& deadline) override;
    core::Result<CapturedFrame> capture(const core::Deadline& deadline) override;
    core::Result<void> reconnect(const CameraSettings& settings, const core::Deadline& deadline) override;
    void close() noexcept override;

    void inject_capture_failure(std::uint32_t capture_call_1based, FaultKind kind);
    void inject_capture_delay(std::uint32_t capture_call_1based, std::chrono::milliseconds delay);
    void clear_faults();

    CallCounter enumerate_call_count;
    CallCounter open_call_count;
    CallCounter capture_call_count;
    CallCounter reconnect_call_count;
    CallCounter close_call_count;

private:
    SyntheticCameraConfig config_;
    FaultInjector faults_;
    bool open_ = false;
    bool disconnected_ = false;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::uint64_t sequence_ = 0;
};

}  // namespace cvforwin::camera

#endif /* CVFORWIN_SRC_CAMERA_TEST_BACKENDS_SYNTHETIC_CAMERA_BACKEND_H_ */

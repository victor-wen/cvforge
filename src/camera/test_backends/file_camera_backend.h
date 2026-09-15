/*
 * Deterministic file camera backend for hardware-free tests and CI.
 *
 * Frames are decoded with OpenCV imgcodecs in the configured order and
 * normalized to owned BGR8 pixels. The backend also exposes the shared fault
 * injection API (disconnect, capture error, bounded delay) and call counters.
 */

#ifndef CVFORWIN_SRC_CAMERA_TEST_BACKENDS_FILE_CAMERA_BACKEND_H_
#define CVFORWIN_SRC_CAMERA_TEST_BACKENDS_FILE_CAMERA_BACKEND_H_

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "camera/camera_backend.h"
#include "camera/test_backends/test_backend_support.h"

namespace cvforwin::camera {

struct FileCameraConfig {
    CameraDescriptor descriptor;
    /* Absolute paths, decoded in order; the first frame captured is frame_paths[0]. */
    std::vector<std::string> frame_paths;
};

class FileCameraBackend final : public ICameraBackend {
public:
    explicit FileCameraBackend(FileCameraConfig config);

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
    FileCameraConfig config_;
    FaultInjector faults_;
    bool open_ = false;
    bool disconnected_ = false;
    std::size_t next_frame_ = 0;
    std::uint64_t sequence_ = 0;
};

}  // namespace cvforwin::camera

#endif /* CVFORWIN_SRC_CAMERA_TEST_BACKENDS_FILE_CAMERA_BACKEND_H_ */

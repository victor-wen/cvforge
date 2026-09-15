#include "camera/test_backends/file_camera_backend.h"

#include <cstddef>
#include <utility>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace cvforwin::camera {

namespace {

constexpr std::string_view kFileBackendKey = "file";

}  // namespace

FileCameraBackend::FileCameraBackend(FileCameraConfig config)
    : config_(std::move(config))
{
}

std::string_view FileCameraBackend::backend_key() const noexcept
{
    return config_.descriptor.backend_key.empty() ? kFileBackendKey : std::string_view{config_.descriptor.backend_key};
}

core::Result<std::vector<CameraDescriptor>> FileCameraBackend::enumerate(const core::Deadline&)
{
    enumerate_call_count.increment();
    return std::vector<CameraDescriptor>{config_.descriptor};
}

core::Result<void> FileCameraBackend::open(const CameraDescriptor& descriptor, const CameraSettings&, const core::Deadline&)
{
    open_call_count.increment();
    if (!descriptors_equal(descriptor, config_.descriptor)) {
        return core::make_failure(core::Status::camera_not_found, core::ErrorCode::descriptor_mismatch,
                                  "file camera descriptor does not match the configured device");
    }
    open_ = true;
    disconnected_ = false;
    next_frame_ = 0;
    sequence_ = 0;
    return core::Result<void>{};
}

core::Result<CapturedFrame> FileCameraBackend::capture(const core::Deadline& deadline)
{
    capture_call_count.increment();

    if (disconnected_) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::camera_disconnected,
                                  "file camera is disconnected");
    }
    if (!open_) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::camera_not_open,
                                  "file camera is not open");
    }

    const auto outcome = faults_.apply(capture_call_count.value(), deadline);
    if (outcome.disconnected) {
        disconnected_ = true;
    }
    if (outcome.failure.has_value()) {
        return *outcome.failure;
    }

    if (next_frame_ >= config_.frame_paths.size()) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::frames_exhausted,
                                  "file camera has no frames left");
    }

    const std::string& path = config_.frame_paths[next_frame_];
    const cv::Mat decoded = cv::imread(path, cv::IMREAD_UNCHANGED);
    if (decoded.empty()) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::capture_failed,
                                  "file camera frame could not be decoded: " + path);
    }

    cv::Mat pixels;
    if (decoded.channels() == 1) {
        cv::cvtColor(decoded, pixels, cv::COLOR_GRAY2BGR);
    } else if (decoded.channels() == 3) {
        pixels = decoded;
    } else if (decoded.channels() == 4) {
        cv::cvtColor(decoded, pixels, cv::COLOR_BGRA2BGR);
    } else {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::capture_failed,
                                  "file camera frame has an unsupported channel count: " + path);
    }

    CapturedFrame frame;
    frame.pixels = pixels;
    frame.metadata.sequence = sequence_;
    frame.metadata.captured_at = std::chrono::steady_clock::now();
    frame.metadata.width = static_cast<std::uint32_t>(pixels.cols);
    frame.metadata.height = static_cast<std::uint32_t>(pixels.rows);
    frame.metadata.pixel_format = PixelFormat::bgr8;
    ++sequence_;
    ++next_frame_;
    return frame;
}

core::Result<void> FileCameraBackend::reconnect(const CameraSettings&, const core::Deadline&)
{
    reconnect_call_count.increment();
    if (!open_) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::camera_not_open,
                                  "file camera is not open");
    }
    disconnected_ = false;
    return core::Result<void>{};
}

void FileCameraBackend::close() noexcept
{
    close_call_count.increment();
    open_ = false;
    disconnected_ = false;
}

void FileCameraBackend::inject_capture_failure(std::uint32_t capture_call_1based, FaultKind kind)
{
    faults_.inject_failure(capture_call_1based, kind);
}

void FileCameraBackend::inject_capture_delay(std::uint32_t capture_call_1based, std::chrono::milliseconds delay)
{
    faults_.inject_delay(capture_call_1based, delay);
}

void FileCameraBackend::clear_faults()
{
    faults_.clear();
    enumerate_call_count.reset();
    open_call_count.reset();
    capture_call_count.reset();
    reconnect_call_count.reset();
    close_call_count.reset();
}

}  // namespace cvforwin::camera

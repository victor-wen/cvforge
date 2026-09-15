#include "camera/test_backends/synthetic_camera_backend.h"

#include <utility>

#include <opencv2/core.hpp>

namespace cvforwin::camera {

namespace {

constexpr std::string_view kSyntheticBackendKey = "synthetic";

/*
 * Frozen frame formula:
 *   pixel(x, y) = BGR((x + y + seq) & 0xFF, (x + 2y + seq) & 0xFF, (2x + y + seq) & 0xFF)
 */
cv::Mat make_synthetic_frame(std::uint32_t width, std::uint32_t height, std::uint64_t sequence)
{
    cv::Mat frame(static_cast<int>(height), static_cast<int>(width), CV_8UC3);
    for (int y = 0; y < frame.rows; ++y) {
        auto* row = frame.ptr<cv::Vec3b>(y);
        for (int x = 0; x < frame.cols; ++x) {
            const auto ux = static_cast<std::uint64_t>(x);
            const auto uy = static_cast<std::uint64_t>(y);
            const auto blue = static_cast<unsigned char>((ux + uy + sequence) & 0xFFu);
            const auto green = static_cast<unsigned char>((ux + 2u * uy + sequence) & 0xFFu);
            const auto red = static_cast<unsigned char>((2u * ux + uy + sequence) & 0xFFu);
            row[x] = cv::Vec3b(blue, green, red);
        }
    }
    return frame;
}

}  // namespace

SyntheticCameraBackend::SyntheticCameraBackend(SyntheticCameraConfig config)
    : config_(std::move(config))
{
}

std::string_view SyntheticCameraBackend::backend_key() const noexcept
{
    return config_.descriptor.backend_key.empty() ? kSyntheticBackendKey : std::string_view{config_.descriptor.backend_key};
}

core::Result<std::vector<CameraDescriptor>> SyntheticCameraBackend::enumerate(const core::Deadline&)
{
    enumerate_call_count.increment();
    return std::vector<CameraDescriptor>{config_.descriptor};
}

core::Result<void> SyntheticCameraBackend::open(const CameraDescriptor& descriptor, const CameraSettings& settings,
                                                const core::Deadline&)
{
    open_call_count.increment();
    if (!descriptors_equal(descriptor, config_.descriptor)) {
        return core::make_failure(core::Status::camera_not_found, core::ErrorCode::descriptor_mismatch,
                                  "synthetic camera descriptor does not match the configured device");
    }

    width_ = settings.width != 0 ? settings.width : config_.width;
    height_ = settings.height != 0 ? settings.height : config_.height;
    sequence_ = 0;
    disconnected_ = false;
    open_ = true;
    return core::Result<void>{};
}

core::Result<CapturedFrame> SyntheticCameraBackend::capture(const core::Deadline& deadline)
{
    capture_call_count.increment();

    if (disconnected_) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::camera_disconnected,
                                  "synthetic camera is disconnected");
    }
    if (!open_) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::camera_not_open,
                                  "synthetic camera is not open");
    }

    const auto outcome = faults_.apply(capture_call_count.value(), deadline);
    if (outcome.disconnected) {
        disconnected_ = true;
    }
    if (outcome.failure.has_value()) {
        return *outcome.failure;
    }

    const auto sequence = sequence_;
    CapturedFrame frame;
    frame.pixels = make_synthetic_frame(width_, height_, sequence);
    frame.metadata.sequence = sequence;
    frame.metadata.captured_at = std::chrono::steady_clock::now();
    frame.metadata.width = width_;
    frame.metadata.height = height_;
    frame.metadata.pixel_format = PixelFormat::bgr8;
    ++sequence_;
    return frame;
}

core::Result<void> SyntheticCameraBackend::reconnect(const CameraSettings&, const core::Deadline&)
{
    reconnect_call_count.increment();
    if (!open_) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::camera_not_open,
                                  "synthetic camera is not open");
    }
    disconnected_ = false;
    return core::Result<void>{};
}

void SyntheticCameraBackend::close() noexcept
{
    close_call_count.increment();
    open_ = false;
    disconnected_ = false;
}

void SyntheticCameraBackend::inject_capture_failure(std::uint32_t capture_call_1based, FaultKind kind)
{
    faults_.inject_failure(capture_call_1based, kind);
}

void SyntheticCameraBackend::inject_capture_delay(std::uint32_t capture_call_1based, std::chrono::milliseconds delay)
{
    faults_.inject_delay(capture_call_1based, delay);
}

void SyntheticCameraBackend::clear_faults()
{
    faults_.clear();
    enumerate_call_count.reset();
    open_call_count.reset();
    capture_call_count.reset();
    reconnect_call_count.reset();
    close_call_count.reset();
}

}  // namespace cvforwin::camera

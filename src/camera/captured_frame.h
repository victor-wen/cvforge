/*
 * Backend-neutral captured-frame model.
 *
 * CapturedFrame carries an owned, normalized BGR8 cv::Mat plus monotonic
 * metadata. It never carries a backend handle, vendor type, product verdict,
 * or algorithm concept, so algorithms can be written against it directly.
 */

#ifndef CVFORWIN_SRC_CAMERA_CAPTURED_FRAME_H_
#define CVFORWIN_SRC_CAMERA_CAPTURED_FRAME_H_

#include <chrono>
#include <cstdint>

#include <opencv2/core.hpp>

namespace cvforwin::camera {

enum class PixelFormat : std::uint32_t {
    unknown = 0,
    mono8 = 1,
    bgr8 = 2,
    rgb8 = 3,
};

struct FrameMetadata {
    std::uint64_t sequence = 0;
    std::chrono::steady_clock::time_point captured_at;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat pixel_format = PixelFormat::unknown;
};

struct CapturedFrame {
    /* Owned pixels, always normalized to BGR8 (CV_8UC3). */
    cv::Mat pixels;
    FrameMetadata metadata;
};

}  // namespace cvforwin::camera

#endif /* CVFORWIN_SRC_CAMERA_CAPTURED_FRAME_H_ */

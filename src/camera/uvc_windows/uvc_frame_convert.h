/*
 * Windows UVC locked 2D-buffer conversion seam (uvc_windows_backend module).
 *
 * This header freezes the single test seam used by the independent Windows
 * component tests. It describes one already-locked 2D video buffer - base
 * pointer, signed pitch, dimensions, native format, and the reachable byte
 * count - and converts it to an owned, continuous BGR8 cv::Mat.
 *
 * scanline0 always addresses the visual TOP row, exactly as IMF2DBuffer
 * defines it, so source row r lives at scanline0 + r * pitch for both signs of
 * pitch; a negative pitch is bottom-up memory only and never triggers an
 * additional vertical reversal. Every addressed row is validated against the
 * accessible bounds before it is read.
 *
 * accessible_bytes is measured from the lowest addressed byte of the scanned
 * region, not from scanline0: that lowest byte is scanline0 for a non-negative
 * pitch and scanline0 - (height - 1) * |pitch| for a negative pitch. A caller
 * whose lock reports a start pointer and a length must therefore pass the
 * length rebased onto that lowest byte,
 * buffer_start + buffer_length - lowest_addressed, rather than the raw length
 * the lock reports from its start.
 *
 * The header is internal: it is not part of the public C ABI, it is guarded by
 * _WIN32, and no Media Foundation type appears in it. uvc_backend.cpp routes
 * its Lock2D/Lock2DSize conversion through decode_locked_buffer_to_bgr8 so the
 * seam and production share one code path.
 */

#ifndef CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_FRAME_CONVERT_H_
#define CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_FRAME_CONVERT_H_

#if defined(_WIN32)

#include <cstddef>
#include <cstdint>

#include <opencv2/core.hpp>

#include "core/error.h"
#include "core/result.h"

namespace cvforwin::camera::uvc {

enum class NativePixelFormat : std::uint32_t {
    rgb24 = 1,
    rgb32 = 2,
    argb32 = 3,
    yuy2 = 4,
    uyvy = 5,
    nv12 = 6,
    i420 = 7,
    yv12 = 8,
};

struct Locked2DBufferView {
    const std::uint8_t* scanline0 = nullptr; /* visual top row, from the 2D lock */
    std::int32_t pitch = 0; /* bytes per row; may be negative */
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    NativePixelFormat format = NativePixelFormat::rgb24;
    std::size_t accessible_bytes = 0; /* reachable bytes from the lowest addressed byte */
    bool accessible_bytes_known = false; /* true when Lock2DSize bounded it */
};

core::Result<cv::Mat> decode_locked_buffer_to_bgr8(const Locked2DBufferView& view);

}  // namespace cvforwin::camera::uvc

#endif /* defined(_WIN32) */

#endif /* CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_FRAME_CONVERT_H_ */

/*
 * Windows UVC locked 2D-buffer conversion (uvc_windows_backend module).
 *
 * Implements the frozen seam decode_locked_buffer_to_bgr8(): one already-locked
 * IMF2DBuffer/IMF2DBuffer2 view (scanline0, signed pitch, dimensions, native
 * format, accessible length) is copied into an owned, continuous BGR8 cv::Mat.
 *
 * Row model (FR-019): scanline0 is the visual TOP row, so visual row r lives at
 * scanline0 + r * pitch for both signs of pitch. A negative pitch is bottom-up
 * memory only; it is applied exactly once and never triggers a second vertical
 * reversal. Every addressed row is validated against the accessible length
 * before it is read, and all extent arithmetic is overflow-checked.
 *
 * The pure row arithmetic and the full-range YUV color helper live in
 * uvc_frame_math.h so they can be unit-tested on a portable host. This
 * translation unit is Windows-only and never exposes a Media Foundation type.
 */

#if defined(_WIN32)

#include "camera/uvc_windows/uvc_frame_convert.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "camera/uvc_windows/uvc_frame_math.h"
#include "core/error.h"
#include "core/status.h"

namespace cvforwin::camera::uvc {

namespace {

using detail::Bgr8;

core::Failure locked_failure(std::string message)
{
    return core::make_failure(core::Status::camera_io, core::ErrorCode::capture_failed, std::move(message));
}

/* Address of visual row `row`; pitch is applied exactly once and may be negative. */
const std::uint8_t* packed_row_pointer(const Locked2DBufferView& view, std::uint32_t row) noexcept
{
    const std::int64_t offset = static_cast<std::int64_t>(row) * static_cast<std::int64_t>(view.pitch);
    return view.scanline0 + static_cast<std::ptrdiff_t>(offset);
}

enum class PackedKind {
    rgb24,
    rgbx32,
    yuy2,
    uyvy,
};

bool packed_layout(NativePixelFormat format, std::uint32_t width, PackedKind& kind, std::size_t& row_bytes) noexcept
{
    std::size_t bytes_per_pixel = 0;
    switch (format) {
    case NativePixelFormat::rgb24:
        kind = PackedKind::rgb24;
        bytes_per_pixel = 3u;
        break;
    case NativePixelFormat::rgb32:
    case NativePixelFormat::argb32:
        kind = PackedKind::rgbx32;
        bytes_per_pixel = 4u;
        break;
    case NativePixelFormat::yuy2:
        kind = PackedKind::yuy2;
        bytes_per_pixel = 2u;
        break;
    case NativePixelFormat::uyvy:
        kind = PackedKind::uyvy;
        bytes_per_pixel = 2u;
        break;
    default:
        return false;
    }
    return detail::checked_mul(static_cast<std::size_t>(width), bytes_per_pixel, row_bytes);
}

/* Copies one packed row into `destination`, converting to BGR8. */
void convert_packed_row(const std::uint8_t* source, std::uint8_t* destination, std::uint32_t width, PackedKind kind)
{
    switch (kind) {
    case PackedKind::rgb24:
        /*
         * D3DFMT_R8G8B8 stores B,G,R in memory, which is already the output
         * BGR8 order; the row is copied unchanged with no channel swap. This
         * matches the authoritative production behavior (a direct copy).
         */
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::size_t index = static_cast<std::size_t>(column) * 3u;
            destination[index] = source[index];
            destination[index + 1u] = source[index + 1u];
            destination[index + 2u] = source[index + 2u];
        }
        break;
    case PackedKind::rgbx32:
        for (std::uint32_t column = 0; column < width; ++column) {
            const std::size_t source_index = static_cast<std::size_t>(column) * 4u;
            const std::size_t out = static_cast<std::size_t>(column) * 3u;
            destination[out] = source[source_index];
            destination[out + 1u] = source[source_index + 1u];
            destination[out + 2u] = source[source_index + 2u];
        }
        break;
    case PackedKind::yuy2:
        for (std::uint32_t column = 0; column + 1u < width; column += 2u) {
            const std::size_t index = static_cast<std::size_t>(column) * 2u;
            const std::uint8_t u = source[index + 1u];
            const std::uint8_t v = source[index + 3u];
            const Bgr8 first = detail::yuv_to_bgr_full(source[index], u, v);
            const Bgr8 second = detail::yuv_to_bgr_full(source[index + 2u], u, v);
            const std::size_t out = static_cast<std::size_t>(column) * 3u;
            destination[out] = first.b;
            destination[out + 1u] = first.g;
            destination[out + 2u] = first.r;
            destination[out + 3u] = second.b;
            destination[out + 4u] = second.g;
            destination[out + 5u] = second.r;
        }
        break;
    case PackedKind::uyvy:
        for (std::uint32_t column = 0; column + 1u < width; column += 2u) {
            const std::size_t index = static_cast<std::size_t>(column) * 2u;
            const std::uint8_t u = source[index];
            const std::uint8_t v = source[index + 2u];
            const Bgr8 first = detail::yuv_to_bgr_full(source[index + 1u], u, v);
            const Bgr8 second = detail::yuv_to_bgr_full(source[index + 3u], u, v);
            const std::size_t out = static_cast<std::size_t>(column) * 3u;
            destination[out] = first.b;
            destination[out + 1u] = first.g;
            destination[out + 2u] = first.r;
            destination[out + 3u] = second.b;
            destination[out + 4u] = second.g;
            destination[out + 5u] = second.r;
        }
        break;
    }
}

core::Result<cv::Mat> decode_packed(const Locked2DBufferView& view, PackedKind kind, std::size_t row_bytes)
{
    if ((kind == PackedKind::yuy2 || kind == PackedKind::uyvy) && (view.width % 2u) != 0u) {
        return locked_failure("the locked 4:2:2 buffer has an odd width");
    }
    if (view.pitch == 0) {
        return locked_failure("the locked buffer row pitch is zero");
    }
    std::size_t magnitude = 0;
    if (!detail::pitch_magnitude(view.pitch, magnitude) || magnitude < row_bytes) {
        return locked_failure("the locked buffer row pitch is smaller than one packed row");
    }
    std::size_t extent = 0;
    if (!detail::packed_region_extent(view.pitch, view.height, row_bytes, extent)) {
        return locked_failure("the locked buffer extent overflows the addressable range");
    }
    if (view.accessible_bytes_known) {
        if (view.accessible_bytes < extent) {
            return locked_failure("the locked Media Foundation buffer is smaller than the required frame extent");
        }
        for (std::uint32_t row = 0; row < view.height; ++row) {
            if (!detail::packed_row_fits(row, view.pitch, view.height, row_bytes, view.accessible_bytes)) {
                return locked_failure("a locked buffer row lies outside the accessible region");
            }
        }
    }

    cv::Mat frame(static_cast<int>(view.height), static_cast<int>(view.width), CV_8UC3);
    for (std::uint32_t row = 0; row < view.height; ++row) {
        const std::uint8_t* source = packed_row_pointer(view, row);
        std::uint8_t* destination = frame.ptr<std::uint8_t>(static_cast<int>(row));
        convert_packed_row(source, destination, view.width, kind);
    }
    return frame;
}

core::Result<cv::Mat> decode_nv12(const Locked2DBufferView& view)
{
    if ((view.width % 2u) != 0u || (view.height % 2u) != 0u) {
        return locked_failure("the locked NV12 buffer is not an even-sized frame");
    }
    if (view.pitch <= 0 || (view.pitch % 2) != 0) {
        return locked_failure("the locked NV12 buffer is not a top-down even-stride frame");
    }
    const std::size_t luma_row_bytes = static_cast<std::size_t>(view.width);
    const std::uint32_t rows = view.height + view.height / 2u;
    std::size_t extent = 0;
    if (!detail::checked_mul(static_cast<std::size_t>(rows - 1u), static_cast<std::size_t>(view.pitch), extent) ||
        !detail::checked_add(extent, luma_row_bytes, extent)) {
        return locked_failure("the locked NV12 buffer extent overflows the addressable range");
    }
    if (view.accessible_bytes_known) {
        if (view.accessible_bytes < extent) {
            return locked_failure("the locked Media Foundation buffer is smaller than the required NV12 extent");
        }
        for (std::uint32_t row = 0; row < rows; ++row) {
            std::size_t offset = 0;
            std::size_t end = 0;
            if (!detail::checked_mul(static_cast<std::size_t>(row), static_cast<std::size_t>(view.pitch), offset) ||
                !detail::checked_add(offset, luma_row_bytes, end) || end > view.accessible_bytes) {
                return locked_failure("a locked NV12 row lies outside the accessible region");
            }
        }
    }

    cv::Mat planar(static_cast<int>(rows), static_cast<int>(view.width), CV_8UC1);
    const std::ptrdiff_t stride = static_cast<std::ptrdiff_t>(view.pitch);
    for (std::uint32_t row = 0; row < rows; ++row) {
        const std::uint8_t* source = view.scanline0 + static_cast<std::ptrdiff_t>(row) * stride;
        std::memcpy(planar.ptr(static_cast<int>(row)), source, luma_row_bytes);
    }
    cv::Mat pixels;
    cv::cvtColor(planar, pixels, cv::COLOR_YUV2BGR_NV12);
    return pixels;
}

core::Result<cv::Mat> decode_planar_420(const Locked2DBufferView& view, NativePixelFormat format)
{
    if ((view.width % 2u) != 0u || (view.height % 2u) != 0u) {
        return locked_failure("the locked planar 4:2:0 buffer is not an even-sized frame");
    }
    if (view.pitch <= 0 || (view.pitch % 2) != 0) {
        return locked_failure("the locked planar 4:2:0 buffer is not a top-down even-stride frame");
    }
    const std::size_t luma_row_bytes = static_cast<std::size_t>(view.width);
    const std::size_t chroma_row_bytes = luma_row_bytes / 2u;
    const std::uint32_t luma_rows = view.height;
    const std::uint32_t chroma_rows = view.height / 2u;
    const std::size_t luma_stride = static_cast<std::size_t>(view.pitch);
    const std::size_t chroma_stride = luma_stride / 2u;

    std::size_t extent = 0;
    std::size_t chroma_extent = 0;
    if (!detail::checked_mul(static_cast<std::size_t>(luma_rows), luma_stride, extent) ||
        !detail::checked_mul(static_cast<std::size_t>(luma_rows - 1u), chroma_stride, chroma_extent) ||
        !detail::checked_add(extent, chroma_extent, extent) ||
        !detail::checked_add(extent, chroma_row_bytes, extent)) {
        return locked_failure("the locked planar 4:2:0 buffer extent overflows the addressable range");
    }
    if (view.accessible_bytes_known) {
        if (view.accessible_bytes < extent) {
            return locked_failure("the locked Media Foundation buffer is smaller than the required planar extent");
        }
        const std::size_t chroma_base = static_cast<std::size_t>(luma_rows) * luma_stride;
        for (std::uint32_t row = 0; row < luma_rows; ++row) {
            std::size_t offset = 0;
            std::size_t end = 0;
            if (!detail::checked_mul(static_cast<std::size_t>(row), luma_stride, offset) ||
                !detail::checked_add(offset, luma_row_bytes, end) || end > view.accessible_bytes) {
                return locked_failure("a locked planar luma row lies outside the accessible region");
            }
        }
        for (std::uint32_t row = 0; row < chroma_rows; ++row) {
            std::size_t offset = 0;
            std::size_t end = 0;
            if (!detail::checked_mul(static_cast<std::size_t>(row), chroma_stride, offset) ||
                !detail::checked_add(chroma_base, offset, end) ||
                !detail::checked_add(end, chroma_row_bytes, end) || end > view.accessible_bytes) {
                return locked_failure("a locked planar chroma row lies outside the accessible region");
            }
        }
    }

    /*
     * Canonical 4:2:0 layout expected by COLOR_YUV2BGR_I420: luma_rows of Y
     * followed by both chroma planes packed as chroma_rows half-width rows,
     * i.e. height * 3 / 2 rows in total. OpenCV's decoder places U at byte
     * width * height and V at width * height + width * height / 4, so the two
     * chroma planes are contiguous at half width rather than one full-width
     * row each. Writing a full-width row per chroma row walked past the
     * height * 3 / 2 allocation and made OpenCV throw at Mat::ptr.
     */
    cv::Mat planar(static_cast<int>(luma_rows + chroma_rows), static_cast<int>(view.width), CV_8UC1);
    const std::ptrdiff_t luma_pitch = static_cast<std::ptrdiff_t>(view.pitch);
    for (std::uint32_t row = 0; row < luma_rows; ++row) {
        const std::uint8_t* source = view.scanline0 + static_cast<std::ptrdiff_t>(row) * luma_pitch;
        std::memcpy(planar.ptr(static_cast<int>(row)), source, luma_row_bytes);
    }
    const std::uint8_t* first_plane = view.scanline0 + static_cast<std::ptrdiff_t>(luma_rows) * luma_pitch;
    const std::uint8_t* second_plane = first_plane + static_cast<std::ptrdiff_t>(chroma_rows) *
                                                         static_cast<std::ptrdiff_t>(chroma_stride);
    const bool first_plane_is_u = format == NativePixelFormat::i420;
    const std::uint8_t* u_plane = first_plane_is_u ? first_plane : second_plane;
    const std::uint8_t* v_plane = first_plane_is_u ? second_plane : first_plane;
    std::uint8_t* const u_destination = planar.ptr(static_cast<int>(luma_rows));
    std::uint8_t* const v_destination = u_destination + static_cast<std::size_t>(chroma_rows) * chroma_row_bytes;
    for (std::uint32_t row = 0; row < chroma_rows; ++row) {
        const std::ptrdiff_t source_offset =
            static_cast<std::ptrdiff_t>(row) * static_cast<std::ptrdiff_t>(chroma_stride);
        const std::size_t destination_offset = static_cast<std::size_t>(row) * chroma_row_bytes;
        std::memcpy(u_destination + destination_offset, u_plane + source_offset, chroma_row_bytes);
        std::memcpy(v_destination + destination_offset, v_plane + source_offset, chroma_row_bytes);
    }
    cv::Mat pixels;
    cv::cvtColor(planar, pixels, cv::COLOR_YUV2BGR_I420);
    return pixels;
}

}  // namespace

core::Result<cv::Mat> decode_locked_buffer_to_bgr8(const Locked2DBufferView& view)
{
    if (view.scanline0 == nullptr) {
        return locked_failure("the locked Media Foundation buffer has no base pointer");
    }
    if (view.width == 0u || view.height == 0u) {
        return locked_failure("the locked Media Foundation buffer has an empty frame");
    }
    if (view.width > detail::k_max_locked_dimension || view.height > detail::k_max_locked_dimension) {
        return locked_failure("the locked Media Foundation frame exceeds the supported dimension limit");
    }

    try {
        PackedKind kind = PackedKind::rgb24;
        std::size_t row_bytes = 0;
        if (packed_layout(view.format, view.width, kind, row_bytes)) {
            return decode_packed(view, kind, row_bytes);
        }
        switch (view.format) {
        case NativePixelFormat::nv12:
            return decode_nv12(view);
        case NativePixelFormat::i420:
        case NativePixelFormat::yv12:
            return decode_planar_420(view, view.format);
        default:
            return locked_failure("the locked Media Foundation buffer uses an unsupported native format");
        }
    } catch (const cv::Exception&) {
        return locked_failure("OpenCV failed to convert the locked Media Foundation buffer to BGR8");
    }
}

}  // namespace cvforwin::camera::uvc

#endif /* defined(_WIN32) */

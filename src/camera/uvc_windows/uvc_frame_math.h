/*
 * Portable arithmetic and color helpers for the Windows UVC 2D-buffer seam.
 *
 * This header intentionally carries no _WIN32 guard and no Media Foundation or
 * OpenCV type: it is the pure, deterministic part of
 * decode_locked_buffer_to_bgr8(), so the overflow-safe row arithmetic and the
 * full-range YUV-to-BGR8 conversion can be unit-tested on a portable host. The
 * Windows-only translation unit src/camera/uvc_windows/uvc_frame_convert.cpp
 * includes it, and the developer unit tests compile it directly on Linux.
 *
 * Row model: scanline0 always addresses the visual TOP row. Visual row r lives
 * at scanline0 + r * pitch for both signs of pitch, so a negative pitch places
 * later visual rows at lower addresses. The lowest addressed byte of the whole
 * locked region is therefore scanline0 for a positive pitch and
 * scanline0 - (height - 1) * |pitch| for a negative pitch. Validation measures
 * every row against the accessible length from that lowest byte.
 *
 * A lock reports its accessible bounds as a start pointer plus a length from
 * that start. The accessible length the seam consumes is always measured from
 * the lowest addressed byte of the scanned region, so a caller must rebase the
 * reported range onto that byte: accessible = buffer_start + buffer_length -
 * lowest_addressed. packed_lowest_address_offset() and
 * rebase_accessible_extent() below are the overflow-safe helpers for exactly
 * that rebasing.
 */

#ifndef CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_FRAME_MATH_H_
#define CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_FRAME_MATH_H_

#include <cstddef>
#include <cstdint>
#include <limits>

namespace cvforwin::camera::uvc::detail {

/* Matches the backend's supported native-frame dimension limit. */
inline constexpr std::uint32_t k_max_locked_dimension = 16384u;

constexpr bool checked_mul(std::size_t left, std::size_t right, std::size_t& out) noexcept
{
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    out = left * right;
    return true;
}

constexpr bool checked_add(std::size_t left, std::size_t right, std::size_t& out) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    out = left + right;
    return true;
}

/*
 * Bytes-per-row magnitude of a signed pitch. INT32_MIN has no representable
 * positive int32 magnitude, so it is rejected instead of being negated.
 */
constexpr bool pitch_magnitude(std::int32_t pitch, std::size_t& out) noexcept
{
    if (pitch == std::numeric_limits<std::int32_t>::min()) {
        return false;
    }
    out = static_cast<std::size_t>(pitch < 0 ? -static_cast<std::int64_t>(pitch) : static_cast<std::int64_t>(pitch));
    return true;
}

/*
 * Total extent of the locked region that the packed rows address, from the
 * lowest addressed byte through the end of scanline0's row:
 * (height - 1) * |pitch| + row_bytes. Returns false when the arithmetic would
 * overflow size_t. Callers must ensure height >= 1.
 */
constexpr bool packed_region_extent(std::int32_t pitch, std::uint32_t height, std::size_t row_bytes,
                                    std::size_t& extent) noexcept
{
    std::size_t magnitude = 0;
    if (!pitch_magnitude(pitch, magnitude)) {
        return false;
    }
    std::size_t span = 0;
    if (!checked_mul(static_cast<std::size_t>(height - 1u), magnitude, span)) {
        return false;
    }
    return checked_add(span, row_bytes, extent);
}

/*
 * Absolute address of the lowest addressed byte of the scanned region:
 * scanline0 for a non-negative pitch (top-down), scanline0 - (height - 1) *
 * |pitch| for a negative pitch (bottom-up), and scanline0 for a single row.
 * Returns false when height is zero, the descent magnitude is unrepresentable,
 * or the descent would underflow below address zero.
 */
constexpr bool lowest_addressed_address(std::size_t scanline0, std::int32_t pitch, std::uint32_t height,
                                        std::size_t& lowest) noexcept
{
    if (height == 0u) {
        return false;
    }
    std::size_t descent = 0;
    if (pitch < 0) {
        std::size_t magnitude = 0;
        if (!pitch_magnitude(pitch, magnitude) ||
            !checked_mul(static_cast<std::size_t>(height - 1u), magnitude, descent)) {
            return false;
        }
    }
    if (descent > scanline0) {
        return false;
    }
    lowest = scanline0 - descent;
    return true;
}

/*
 * Rebase a lock's reported (buffer_start, buffer_length) range onto a lowest
 * addressed byte, i.e. accessible = buffer_start + buffer_length -
 * lowest_addressed. Returns false when the reported range is empty, does not
 * strictly contain the lowest addressed byte, or the end-of-range computation
 * overflows. `start`, `lowest_addressed` and the result are byte
 * counts/addresses in the same space.
 */
constexpr bool rebase_accessible_extent(std::size_t start, std::size_t lowest_addressed,
                                        std::size_t buffer_length, std::size_t& accessible) noexcept
{
    if (buffer_length == 0u || lowest_addressed < start) {
        return false;
    }
    std::size_t end = 0;
    if (!checked_add(start, buffer_length, end)) {
        return false;
    }
    if (lowest_addressed >= end) {
        return false;
    }
    accessible = end - lowest_addressed;
    return true;
}

/*
 * Composite of the two helpers above: given a 2D lock's scanline0, pitch and
 * frame height together with its reported (buffer_start, buffer_length) range,
 * computes the accessible byte count measured from the lowest addressed byte.
 * Returns false when the height is zero, the arithmetic is unrepresentable, or
 * the reported range does not strictly contain the addressed region. This is
 * the exact basis the seam consumes, so the backend only widens pointers to
 * whole addresses and calls it.
 */
constexpr bool lock_accessible_extent(std::size_t scanline0, std::int32_t pitch, std::uint32_t height,
                                      std::size_t buffer_start, std::size_t buffer_length,
                                      std::size_t& accessible) noexcept
{
    std::size_t lowest = 0;
    if (!lowest_addressed_address(scanline0, pitch, height, lowest)) {
        return false;
    }
    return rebase_accessible_extent(buffer_start, lowest, buffer_length, accessible);
}

/*
 * Offset of visual row `row` from the lowest addressed byte of the region, or
 * false when the arithmetic overflows. Callers must ensure row < height and
 * height >= 1.
 */
constexpr bool packed_row_region_offset(std::uint32_t row, std::int32_t pitch, std::uint32_t height,
                                        std::size_t& offset) noexcept
{
    std::size_t magnitude = 0;
    if (!pitch_magnitude(pitch, magnitude)) {
        return false;
    }
    const std::uint32_t from_lowest = pitch < 0 ? (height - 1u - row) : row;
    return checked_mul(static_cast<std::size_t>(from_lowest), magnitude, offset);
}

/*
 * True when visual row `row` (start and end) lies inside `accessible_bytes`
 * measured from the lowest addressed byte. Callers must ensure height >= 1.
 */
constexpr bool packed_row_fits(std::uint32_t row, std::int32_t pitch, std::uint32_t height, std::size_t row_bytes,
                               std::size_t accessible_bytes) noexcept
{
    std::size_t offset = 0;
    if (!packed_row_region_offset(row, pitch, height, offset)) {
        return false;
    }
    std::size_t end = 0;
    if (!checked_add(offset, row_bytes, end)) {
        return false;
    }
    return end <= accessible_bytes;
}

constexpr std::uint8_t clamp_to_byte(int value) noexcept
{
    if (value < 0) {
        return static_cast<std::uint8_t>(0);
    }
    if (value > 255) {
        return static_cast<std::uint8_t>(255);
    }
    return static_cast<std::uint8_t>(value);
}

struct Bgr8 {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
};

/*
 * Full-range BT.601 YUV (Cb=U, Cr=V) to BGR8. Neutral chroma (U = V = 128)
 * maps to exactly (Y, Y, Y), which is the orientation/neutrality contract the
 * component tests pin; OpenCV's COLOR_YUV2BGR_* macros apply the limited
 * (16..235) video range instead and would shift neutral chroma to 130.
 */
constexpr Bgr8 yuv_to_bgr_full(std::uint8_t y, std::uint8_t u, std::uint8_t v) noexcept
{
    const int luma = static_cast<int>(y);
    const int cb = static_cast<int>(u) - 128;
    const int cr = static_cast<int>(v) - 128;
    const int r = luma + ((91881 * cr) >> 16);
    const int g = luma - ((22554 * cb + 46802 * cr) >> 16);
    const int b = luma + ((116130 * cb) >> 16);
    return Bgr8{clamp_to_byte(b), clamp_to_byte(g), clamp_to_byte(r)};
}

}  // namespace cvforwin::camera::uvc::detail

#endif /* CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_FRAME_MATH_H_ */

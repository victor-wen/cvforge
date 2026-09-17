/*
 * CVF-101 independent black-box component tests for the frozen Windows UVC
 * 2D-buffer seam declared in
 * src/camera/uvc_windows/uvc_frame_convert.h.
 *
 * Coverage: brief observable behaviors B1..B6, the boundary_cases, and the
 * negative_cases. Every case builds a synthetic locked buffer in memory and
 * calls only cvforwin::camera::uvc::decode_locked_buffer_to_bgr8(); no camera,
 * no Media Foundation object, and no device is touched. The frozen seam header
 * is the only production surface this file includes.
 *
 * Native packed formats follow the Media Foundation memory layouts fixed by the
 * change contract: rgb24 (D3DFMT_R8G8B8) and rgb32 (X8R8G8B8) store B,G,R in
 * memory, and argb32 stores B,G,R,A. The seam always produces BGR8, so a native
 * memory triple [b, g, r] must map to the identical BGR8 triple (b, g, r) with
 * no channel swap. 8-bit YUV formats use U = V = 128 as neutral; the contract
 * fixes no limited-versus-full range, so those cases assert structural
 * neutrality (equal BGR channels) and orientation instead of a pinned range.
 *
 * The whole translation unit is Windows-only because the seam and the UVC
 * backend are guarded by _WIN32. On portable builds it is an empty translation
 * unit, so the portable suite is unaffected and provides no evidence for this
 * change.
 */

#if defined(_WIN32)

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <opencv2/core.hpp>

#include "camera/uvc_windows/uvc_frame_convert.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

namespace cvf101 {

namespace core = cvforwin::core;
namespace uvc = cvforwin::camera::uvc;

using uvc::Locked2DBufferView;
using uvc::NativePixelFormat;

/* ------------------------------------------------------------------------- */
/* Synthetic locked-buffer construction helpers.                             */
/* ------------------------------------------------------------------------- */

constexpr std::uint8_t kPadByte = 0xEEu;

/*
 * Bytes per packed pixel row. Only the packed formats named by the brief are
 * covered here; planar layouts have their own minimum-layout helper below.
 */
std::size_t packed_row_bytes(NativePixelFormat format, std::uint32_t width)
{
    switch (format) {
    case NativePixelFormat::rgb24:
        return static_cast<std::size_t>(width) * 3u;
    case NativePixelFormat::rgb32:
    case NativePixelFormat::argb32:
        return static_cast<std::size_t>(width) * 4u;
    case NativePixelFormat::yuy2:
    case NativePixelFormat::uyvy:
        return static_cast<std::size_t>(width) * 2u;
    default:
        return 0u;
    }
}

/*
 * The contiguous byte count of the smallest meaningful row unit for a native
 * format. For packed formats this is one packed scanline; for planar formats
 * this is one packed scanline of the Y plane, which is the tightest pitch a
 * single-row planar layout can transport. It is used only to size synthetic
 * buffers and to compute a valid pitch for width/height edge cases; it never
 * fixes any decoder behavior beyond the seam's documented input layout.
 */
std::size_t native_min_row_bytes(NativePixelFormat format, std::uint32_t width)
{
    switch (format) {
    case NativePixelFormat::rgb24:
        return static_cast<std::size_t>(width) * 3u;
    case NativePixelFormat::rgb32:
    case NativePixelFormat::argb32:
        return static_cast<std::size_t>(width) * 4u;
    case NativePixelFormat::yuy2:
    case NativePixelFormat::uyvy:
        return static_cast<std::size_t>(width) * 2u;
    case NativePixelFormat::nv12:
    case NativePixelFormat::i420:
    case NativePixelFormat::yv12:
        return static_cast<std::size_t>(width);
    default:
        return 0u;
    }
}

/*
 * Fills one packed row with a neutral-gray pixel. A gray pixel maps to BGR
 * (g, g, g) under any documented RGB or YUV (U=V=128) conversion, so the row
 * stays channel-order independent.
 */
void fill_gray_row(std::uint8_t* row, NativePixelFormat format, std::uint32_t width, std::uint8_t gray)
{
    const std::size_t bytes = native_min_row_bytes(format, width);
    switch (format) {
    case NativePixelFormat::rgb24:
        for (std::size_t index = 0; index + 2u < bytes; index += 3u) {
            row[index] = gray;
            row[index + 1u] = gray;
            row[index + 2u] = gray;
        }
        break;
    case NativePixelFormat::rgb32:
    case NativePixelFormat::argb32:
        for (std::size_t index = 0; index + 3u < bytes; index += 4u) {
            row[index] = gray;
            row[index + 1u] = gray;
            row[index + 2u] = gray;
            row[index + 3u] = 0u;
        }
        break;
    case NativePixelFormat::yuy2:
        for (std::size_t index = 0; index + 3u < bytes; index += 4u) {
            row[index] = gray;
            row[index + 1u] = 128u;
            row[index + 2u] = gray;
            row[index + 3u] = 128u;
        }
        break;
    case NativePixelFormat::uyvy:
        for (std::size_t index = 0; index + 3u < bytes; index += 4u) {
            row[index] = 128u;
            row[index + 1u] = gray;
            row[index + 2u] = 128u;
            row[index + 3u] = gray;
        }
        break;
    case NativePixelFormat::nv12:
    case NativePixelFormat::i420:
    case NativePixelFormat::yv12:
        /* Planar: the row pointer addresses the Y plane only; U and V planes are
         * neutral (128) in the synthetic storage and are not touched here. */
        for (std::size_t index = 0; index < bytes; ++index) {
            row[index] = gray;
        }
        break;
    default:
        break;
    }
}

/*
 * Fills one rgb24 row with a uniform pixel whose native memory bytes are
 * (b, g, r). Media Foundation D3DFMT_R8G8B8 stores B,G,R in memory, so the first
 * byte is the blue channel and the resulting BGR8 pixel is (b, g, r) unchanged.
 */
void fill_rgb24_row(std::uint8_t* row, std::uint32_t width, std::uint8_t blue, std::uint8_t green, std::uint8_t red)
{
    for (std::uint32_t column = 0; column < width; ++column) {
        row[static_cast<std::size_t>(column) * 3u] = blue;
        row[static_cast<std::size_t>(column) * 3u + 1u] = green;
        row[static_cast<std::size_t>(column) * 3u + 2u] = red;
    }
}

/*
 * Fills one 32-bit packed row (rgb32 / argb32) with a uniform pixel whose native
 * memory bytes are (b, g, r, a). Both layouts store B,G,R in the low three
 * bytes; they differ only in the meaning of the ignored fourth byte.
 */
void fill_bgr32_row(std::uint8_t* row,
                    std::uint32_t width,
                    std::uint8_t blue,
                    std::uint8_t green,
                    std::uint8_t red,
                    std::uint8_t alpha)
{
    for (std::uint32_t column = 0; column < width; ++column) {
        const std::size_t index = static_cast<std::size_t>(column) * 4u;
        row[index] = blue;
        row[index + 1u] = green;
        row[index + 2u] = red;
        row[index + 3u] = alpha;
    }
}

/*
 * A synthetic 2D lock. scanline0 always addresses the visual top row; a
 * negative pitch places the following visual rows at lower addresses, exactly
 * as a bottom-up IMF2DBuffer does.
 */
class SyntheticLock {
public:
    SyntheticLock(NativePixelFormat format, std::uint32_t width, std::uint32_t height, std::int32_t pitch)
        : format_(format), width_(width), height_(height), pitch_(pitch), row_bytes_(packed_row_bytes(format, width))
    {
        step_ = pitch_ < 0 ? static_cast<std::size_t>(-static_cast<std::int64_t>(pitch_))
                           : static_cast<std::size_t>(pitch_);
        const std::size_t row_count = height_ == 0u ? 0u : static_cast<std::size_t>(height_ - 1u);
        span_ = row_count * step_ + row_bytes_;
        storage_.assign(span_ + kGuardBytes, kPadByte);
        top_offset_ = (pitch_ < 0 && height_ > 0u) ? row_count * step_ : 0u;
    }

    std::uint8_t* visual_row(std::uint32_t row)
    {
        const std::size_t distance = static_cast<std::size_t>(row) * step_;
        if (pitch_ < 0) {
            return storage_.data() + top_offset_ - distance;
        }
        return storage_.data() + top_offset_ + distance;
    }

    void fill(std::uint32_t row, std::uint8_t gray)
    {
        fill_gray_row(visual_row(row), format_, width_, gray);
    }

    void fill_rgb24(std::uint32_t row, std::uint8_t blue, std::uint8_t green, std::uint8_t red)
    {
        fill_rgb24_row(visual_row(row), width_, blue, green, red);
    }

    void fill_bgr32(std::uint32_t row, std::uint8_t blue, std::uint8_t green, std::uint8_t red, std::uint8_t alpha)
    {
        fill_bgr32_row(visual_row(row), width_, blue, green, red, alpha);
    }

    void fill_padding()
    {
        for (std::uint32_t row = 0; row < height_; ++row) {
            std::uint8_t* start = visual_row(row);
            for (std::size_t byte = row_bytes_; byte < step_; ++byte) {
                start[byte] = kPadByte;
            }
        }
    }

    std::size_t row_bytes() const noexcept
    {
        return row_bytes_;
    }

    std::size_t span() const noexcept
    {
        return span_;
    }

    std::size_t step() const noexcept
    {
        return step_;
    }

    Locked2DBufferView view(bool accessible_known, std::size_t accessible) const
    {
        Locked2DBufferView locked;
        locked.scanline0 = storage_.data() + top_offset_;
        locked.pitch = pitch_;
        locked.width = width_;
        locked.height = height_;
        locked.format = format_;
        locked.accessible_bytes = accessible;
        locked.accessible_bytes_known = accessible_known;
        return locked;
    }

    /*
     * Bounds are attributed exactly at the minimum addressed extent. Cases
     * that isolate orientation or a format therefore still present a
     * well-formed, fully contained lock, and the bounds-sensitive behavior is
     * asserted separately by the B5 and boundary cases.
     */
    Locked2DBufferView bounded_view() const
    {
        return view(true, span_);
    }

private:
    static constexpr std::size_t kGuardBytes = 64u;

    NativePixelFormat format_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::int32_t pitch_;
    std::size_t row_bytes_ = 0u;
    std::size_t step_ = 0u;
    std::size_t span_ = 0u;
    std::size_t top_offset_ = 0u;
    std::vector<std::uint8_t> storage_;
};

/*
 * A planar 2D lock (nv12/i420/yv12). Its whole allocation is initialized to
 * neutral chroma (0x80 = U = V = 128) before the caller paints the addressed
 * luma rows, so whichever planar chroma pointer a decoder derives from this
 * same lock reads neutral data. It deliberately does not reuse SyntheticLock:
 * the packed helper pre-fills guard bytes with 0xEE, which is meaningful input
 * for the padding cases but not a neutral chroma value.
 *
 * The geometry matches the packed helper exactly: scanline0 addresses the
 * visual top luma row, a negative pitch places later visual rows at lower
 * addresses, and the accessible extent covers the whole allocation. It pins no
 * chroma-plane convention and no limited/full range.
 */
class PlanarLock {
public:
    PlanarLock(NativePixelFormat format, std::uint32_t width, std::uint32_t height, std::int32_t pitch)
        : format_(format), width_(width), height_(height), pitch_(pitch)
    {
        step_ = pitch_ < 0 ? static_cast<std::size_t>(-static_cast<std::int64_t>(pitch_))
                           : static_cast<std::size_t>(pitch_);
        const std::size_t row_count = height_ == 0u ? 0u : static_cast<std::size_t>(height_ - 1u);
        luma_span_ = row_count * step_ + static_cast<std::size_t>(width_);
        /* Generous chroma allowance: one U and one V byte per luma pixel. */
        total_ = luma_span_ + 2u * static_cast<std::size_t>(width_) * height_ + kGuardBytes;
        storage_.assign(total_, 0x80u);
        top_offset_ = (pitch_ < 0 && height_ > 0u) ? row_count * step_ : 0u;
    }

    void fill(std::uint32_t row, std::uint8_t luma)
    {
        std::uint8_t* start = visual_row(row);
        for (std::uint32_t column = 0; column < width_; ++column) {
            start[column] = luma;
        }
    }

    Locked2DBufferView view(std::size_t accessible) const
    {
        Locked2DBufferView locked;
        locked.scanline0 = storage_.data() + top_offset_;
        locked.pitch = pitch_;
        locked.width = width_;
        locked.height = height_;
        locked.format = format_;
        locked.accessible_bytes = accessible;
        locked.accessible_bytes_known = true;
        return locked;
    }

    Locked2DBufferView bounded_view() const
    {
        return view(total_);
    }

    std::uint8_t* visual_row(std::uint32_t row)
    {
        const std::size_t distance = static_cast<std::size_t>(row) * step_;
        if (pitch_ < 0) {
            return storage_.data() + top_offset_ - distance;
        }
        return storage_.data() + top_offset_ + distance;
    }

private:
    static constexpr std::size_t kGuardBytes = 64u;

    NativePixelFormat format_;
    std::uint32_t width_;
    std::uint32_t height_;
    std::int32_t pitch_;
    std::size_t step_ = 0u;
    std::size_t luma_span_ = 0u;
    std::size_t total_ = 0u;
    std::size_t top_offset_ = 0u;
    std::vector<std::uint8_t> storage_;
};

/* ------------------------------------------------------------------------- */
/* Assertion helpers.                                                        */
/* ------------------------------------------------------------------------- */

void check_frame_shape(const cv::Mat& frame, std::uint32_t width, std::uint32_t height)
{
    CHECK(frame.type() == CV_8UC3);
    CHECK(frame.data != nullptr);
    CHECK(frame.isContinuous());
    CHECK(frame.rows == static_cast<int>(height));
    CHECK(frame.cols == static_cast<int>(width));
}

void check_uniform_row(const cv::Mat& frame, int row, const cv::Vec3b& expected)
{
    REQUIRE(row >= 0);
    REQUIRE(row < frame.rows);
    for (int column = 0; column < frame.cols; ++column) {
        INFO("frame row " << row << " column " << column);
        CHECK(frame.at<cv::Vec3b>(row, column) == expected);
    }
}

/*
 * True when every pixel of the row has equal B, G, and R channels. With U = V =
 * 128 the YUV formats are neutral, so the converted BGR pixel must be gray.
 * This asserts structural neutrality without pinning a limited-versus-full
 * luminance range, which the change contract does not fix.
 */
bool row_is_neutral(const cv::Mat& frame, int row)
{
    for (int column = 0; column < frame.cols; ++column) {
        const cv::Vec3b pixel = frame.at<cv::Vec3b>(row, column);
        if (pixel[0] != pixel[1] || pixel[1] != pixel[2]) {
            return false;
        }
    }
    return true;
}

/* Mean of the blue channel of a row; used only for monotonic ordering checks. */
int row_luma_proxy(const cv::Mat& frame, int row)
{
    if (frame.cols == 0) {
        return 0;
    }
    int sum = 0;
    for (int column = 0; column < frame.cols; ++column) {
        sum += frame.at<cv::Vec3b>(row, column)[0];
    }
    return sum / frame.cols;
}

/*
 * The documented camera failure for a locked buffer that cannot be decoded.
 * The brief pins camera_io/capture_failed for insufficient accessible length,
 * and the change contract records the same failure for truncated or
 * unattributable layouts.
 */
void check_camera_failure(const core::Result<cv::Mat>& result)
{
    REQUIRE_FALSE(result.has_value());
    CHECK(result.failure().status == core::Status::camera_io);
    CHECK(result.failure().code == core::ErrorCode::capture_failed);
}

/*
 * Three visually asymmetric rows with distinct channels. Each constant is the
 * expected BGR8 triple; its native rgb24 memory bytes are the same three values
 * in the same order (B, G, R), so a channel-swapping decoder fails these.
 */
const cv::Vec3b kBgrA(30, 20, 10);
const cv::Vec3b kBgrMid(60, 50, 40);
const cv::Vec3b kBgrB(100, 150, 200);

/* ------------------------------------------------------------------------- */
/* B1: positive pitch preserves visual row order.                            */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 B1: a top-down buffer returns the visual top row first and the bottom row last",
          "[cvf-101][B1]")
{
    SyntheticLock lock(NativePixelFormat::rgb24, 6u, 3u, 18);
    lock.fill_rgb24(0u, kBgrA[0], kBgrA[1], kBgrA[2]);
    lock.fill_rgb24(1u, kBgrMid[0], kBgrMid[1], kBgrMid[2]);
    lock.fill_rgb24(2u, kBgrB[0], kBgrB[1], kBgrB[2]);

    const auto result = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span()));

    REQUIRE(result.has_value());
    check_frame_shape(result.value(), 6u, 3u);
    check_uniform_row(result.value(), 0, kBgrA);
    check_uniform_row(result.value(), 1, kBgrMid);
    check_uniform_row(result.value(), 2, kBgrB);
}

/* ------------------------------------------------------------------------- */
/* B2: negative pitch reverses memory only, never the visual frame.          */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 B2: a bottom-up buffer (pitch < 0) yields the same visual orientation with no double flip",
          "[cvf-101][B2]")
{
    SyntheticLock lock(NativePixelFormat::rgb24, 6u, 3u, -18);
    lock.fill_rgb24(0u, kBgrA[0], kBgrA[1], kBgrA[2]);
    lock.fill_rgb24(1u, kBgrMid[0], kBgrMid[1], kBgrMid[2]);
    lock.fill_rgb24(2u, kBgrB[0], kBgrB[1], kBgrB[2]);

    const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());

    REQUIRE(result.has_value());
    check_frame_shape(result.value(), 6u, 3u);
    check_uniform_row(result.value(), 0, kBgrA);
    check_uniform_row(result.value(), 1, kBgrMid);
    check_uniform_row(result.value(), 2, kBgrB);
}

/* ------------------------------------------------------------------------- */
/* B3: the same orientation rule holds for RGB24, RGB32, and YUY2.           */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 B3: RGB24, RGB32, and YUY2 all preserve top/bottom orientation for both pitch signs",
          "[cvf-101][B3]")
{
    /*
     * Both pitch signs are checked for each format in one body. Catch2 SECTIONs
     * must not live inside a loop, so the format and pitch sign are explicit
     * parameters of the shared check helper instead.
     */

    /* Packed RGB inputs carry an explicit per-channel value, so the BGR8 triple
     * is exact and range-independent. */
    const auto check_orientation_exact = [](NativePixelFormat format, std::int32_t pitch) {
        const std::uint32_t width = 4u;
        const std::uint32_t height = 3u;
        SyntheticLock lock(format, width, height, pitch);
        lock.fill(0u, 0u);
        lock.fill(1u, 128u);
        lock.fill(2u, 255u);

        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        INFO("format " << static_cast<int>(format) << " pitch " << pitch);
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), width, height);
        check_uniform_row(result.value(), 0, cv::Vec3b(0, 0, 0));
        check_uniform_row(result.value(), 1, cv::Vec3b(128, 128, 128));
        check_uniform_row(result.value(), 2, cv::Vec3b(255, 255, 255));
    };

    /*
     * YUV inputs with U = V = 128 are neutral, but the contract fixes no
     * limited-versus-full luminance range, so only structural neutrality (equal
     * B, G, R) and monotonic ordering (dark row first, bright row last) are
     * asserted, never a specific luma value.
     */
    const auto check_orientation_neutral = [](NativePixelFormat format, std::int32_t pitch) {
        const std::uint32_t width = 4u;
        const std::uint32_t height = 3u;
        SyntheticLock lock(format, width, height, pitch);
        lock.fill(0u, 0u);
        lock.fill(1u, 128u);
        lock.fill(2u, 255u);

        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        INFO("format " << static_cast<int>(format) << " pitch " << pitch);
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), width, height);
        for (int row = 0; row < 3; ++row) {
            CHECK(row_is_neutral(result.value(), row));
        }
        CHECK(row_luma_proxy(result.value(), 0) < row_luma_proxy(result.value(), 1));
        CHECK(row_luma_proxy(result.value(), 1) < row_luma_proxy(result.value(), 2));
    };

    check_orientation_exact(NativePixelFormat::rgb24, 12); /* 4 px * 3 = 12 */
    check_orientation_exact(NativePixelFormat::rgb24, -12);
    check_orientation_exact(NativePixelFormat::rgb32, 16); /* 4 px * 4 = 16 */
    check_orientation_exact(NativePixelFormat::rgb32, -16);
    check_orientation_neutral(NativePixelFormat::yuy2, 8); /* 4 px * 2 = 8  */
    check_orientation_neutral(NativePixelFormat::yuy2, -8);
}

/* ------------------------------------------------------------------------- */
/* B3 channel order: packed B,G,R memory maps to the same BGR8 triple.        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 B3: packed B,G,R native memory decodes to the identical BGR8 channel order",
          "[cvf-101][B3][channel-order]")
{
    /*
     * Distinct blue, green, and red values so a decoder that swaps channels (for
     * example treating rgb24 memory as R,G,B) produces a visibly different BGR
     * triple. This is the regression guard for color correctness.
     */
    SECTION("rgb24 2x1 top-down")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 2u, 1u, 6);
        lock.fill_rgb24(0u, kBgrA[0], kBgrA[1], kBgrA[2]);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 2u, 1u);
        check_uniform_row(result.value(), 0, kBgrA);
    }

    SECTION("rgb24 2x2 bottom-up (pitch < 0)")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 2u, 2u, -6);
        lock.fill_rgb24(0u, kBgrA[0], kBgrA[1], kBgrA[2]);
        lock.fill_rgb24(1u, kBgrB[0], kBgrB[1], kBgrB[2]);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 2u, 2u);
        check_uniform_row(result.value(), 0, kBgrA);
        check_uniform_row(result.value(), 1, kBgrB);
    }

    SECTION("rgb32 2x1 top-down and bottom-up")
    {
        SyntheticLock top(NativePixelFormat::rgb32, 2u, 1u, 8);
        top.fill_bgr32(0u, kBgrB[0], kBgrB[1], kBgrB[2], 0u);
        const auto top_result = uvc::decode_locked_buffer_to_bgr8(top.bounded_view());
        REQUIRE(top_result.has_value());
        check_frame_shape(top_result.value(), 2u, 1u);
        check_uniform_row(top_result.value(), 0, kBgrB);

        SyntheticLock bottom(NativePixelFormat::rgb32, 2u, 1u, -8);
        bottom.fill_bgr32(0u, kBgrB[0], kBgrB[1], kBgrB[2], 0u);
        const auto bottom_result = uvc::decode_locked_buffer_to_bgr8(bottom.bounded_view());
        REQUIRE(bottom_result.has_value());
        check_frame_shape(bottom_result.value(), 2u, 1u);
        check_uniform_row(bottom_result.value(), 0, kBgrB);
    }

    SECTION("argb32 2x1 top-down and bottom-up")
    {
        SyntheticLock top(NativePixelFormat::argb32, 2u, 1u, 8);
        top.fill_bgr32(0u, kBgrMid[0], kBgrMid[1], kBgrMid[2], 0xFFu);
        const auto top_result = uvc::decode_locked_buffer_to_bgr8(top.bounded_view());
        REQUIRE(top_result.has_value());
        check_frame_shape(top_result.value(), 2u, 1u);
        check_uniform_row(top_result.value(), 0, kBgrMid);

        SyntheticLock bottom(NativePixelFormat::argb32, 2u, 1u, -8);
        bottom.fill_bgr32(0u, kBgrMid[0], kBgrMid[1], kBgrMid[2], 0xFFu);
        const auto bottom_result = uvc::decode_locked_buffer_to_bgr8(bottom.bounded_view());
        REQUIRE(bottom_result.has_value());
        check_frame_shape(bottom_result.value(), 2u, 1u);
        check_uniform_row(bottom_result.value(), 0, kBgrMid);
    }
}

/* ------------------------------------------------------------------------- */
/* B4: rows are addressed by pitch; padding never reaches the output.        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 B4: a padded row is decoded by pitch and pad bytes never appear in the output",
          "[cvf-101][B4]")
{
    /* rgb24 width 5 -> 15 packed bytes; pitch 20 leaves 5 pad bytes per row. */
    SyntheticLock lock(NativePixelFormat::rgb24, 5u, 3u, 20);
    lock.fill(0u, 0x10u);
    lock.fill(1u, 0x18u);
    lock.fill(2u, 0x20u);
    lock.fill_padding();

    const auto result = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span()));

    REQUIRE(result.has_value());
    check_frame_shape(result.value(), 5u, 3u);
    check_uniform_row(result.value(), 0, cv::Vec3b(0x10u, 0x10u, 0x10u));
    check_uniform_row(result.value(), 1, cv::Vec3b(0x18u, 0x18u, 0x18u));
    check_uniform_row(result.value(), 2, cv::Vec3b(0x20u, 0x20u, 0x20u));

    bool pad_seen = false;
    for (int row = 0; row < result.value().rows; ++row) {
        for (int column = 0; column < result.value().cols; ++column) {
            const cv::Vec3b pixel = result.value().at<cv::Vec3b>(row, column);
            for (int channel = 0; channel < 3; ++channel) {
                if (pixel[channel] == kPadByte) {
                    pad_seen = true;
                }
            }
        }
    }
    CHECK_FALSE(pad_seen);
}

/* ------------------------------------------------------------------------- */
/* Boundary cases.                                                           */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 boundary: 1x1 packed images decode for positive and negative pitch", "[cvf-101][boundary]")
{
    SECTION("rgb24 1x1 top-down")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 1u, 1u, 3);
        lock.fill_rgb24(0u, kBgrA[0], kBgrA[1], kBgrA[2]);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 1u, 1u);
        check_uniform_row(result.value(), 0, kBgrA);
    }

    SECTION("rgb24 1x1 bottom-up")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 1u, 1u, -3);
        lock.fill_rgb24(0u, kBgrA[0], kBgrA[1], kBgrA[2]);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 1u, 1u);
        check_uniform_row(result.value(), 0, kBgrA);
    }

    SECTION("rgb32 1x1 top-down and bottom-up")
    {
        SyntheticLock top(NativePixelFormat::rgb32, 1u, 1u, 4);
        top.fill(0u, 77u);
        const auto top_result = uvc::decode_locked_buffer_to_bgr8(top.bounded_view());
        REQUIRE(top_result.has_value());
        check_frame_shape(top_result.value(), 1u, 1u);
        check_uniform_row(top_result.value(), 0, cv::Vec3b(77, 77, 77));

        SyntheticLock bottom(NativePixelFormat::rgb32, 1u, 1u, -4);
        bottom.fill(0u, 77u);
        const auto bottom_result = uvc::decode_locked_buffer_to_bgr8(bottom.bounded_view());
        REQUIRE(bottom_result.has_value());
        check_frame_shape(bottom_result.value(), 1u, 1u);
        check_uniform_row(bottom_result.value(), 0, cv::Vec3b(77, 77, 77));
    }
}

TEST_CASE("CVF-101 boundary: height-1 and width-1 edges decode for the packed formats", "[cvf-101][boundary]")
{
    SECTION("rgb24 1x1 is also the height-1 and width-1 edge")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 1u, 1u, 3);
        lock.fill(0u, 55u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 1u, 1u);
        check_uniform_row(result.value(), 0, cv::Vec3b(55, 55, 55));
    }

    SECTION("rgb24 width 1 with height greater than 1")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 1u, 3u, 3);
        lock.fill(0u, 10u);
        lock.fill(2u, 200u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 1u, 3u);
        check_uniform_row(result.value(), 0, cv::Vec3b(10, 10, 10));
        check_uniform_row(result.value(), 2, cv::Vec3b(200, 200, 200));
    }

    SECTION("rgb32 width 1 with height greater than 1")
    {
        SyntheticLock lock(NativePixelFormat::rgb32, 1u, 3u, 4);
        lock.fill(0u, 10u);
        lock.fill(2u, 200u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 1u, 3u);
        check_uniform_row(result.value(), 0, cv::Vec3b(10, 10, 10));
        check_uniform_row(result.value(), 2, cv::Vec3b(200, 200, 200));
    }

    /*
     * A YUY2 macropixel packs two pixels, so the smallest representable width
     * is 2; a width of 1 has no valid packed layout.
     */
    SECTION("yuy2 minimum even width 2 with height 1")
    {
        SyntheticLock lock(NativePixelFormat::yuy2, 2u, 1u, 4);
        lock.fill(0u, 55u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 2u, 1u);
        /* U = V = 128 is neutral; no limited/full range is pinned. */
        CHECK(row_is_neutral(result.value(), 0));
    }
}

TEST_CASE("CVF-101 boundary: accessible length exactly the minimum extent is accepted, one byte short is rejected",
          "[cvf-101][boundary]")
{
    SECTION("rgb24 4x3 pitch 12: minimum extent accepted")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 3u, 12);
        lock.fill(0u, 10u);
        lock.fill(2u, 200u);
        /* (3-1)*12 + 12 = 36 bytes */
        REQUIRE(lock.span() == 36u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span()));
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 4u, 3u);
    }

    SECTION("rgb24 4x3 pitch 12: one byte short is rejected")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 3u, 12);
        lock.fill(0u, 10u);
        lock.fill(2u, 200u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span() - 1u));
        check_camera_failure(result);
    }

    SECTION("rgb32 4x2 pitch 16: minimum extent accepted, one byte short rejected")
    {
        SyntheticLock lock(NativePixelFormat::rgb32, 4u, 2u, 16);
        lock.fill(0u, 10u);
        lock.fill(1u, 200u);
        /* (2-1)*16 + 16 = 32 bytes */
        REQUIRE(lock.span() == 32u);
        const auto accepted = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span()));
        REQUIRE(accepted.has_value());
        check_frame_shape(accepted.value(), 4u, 2u);

        const auto rejected = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span() - 1u));
        check_camera_failure(rejected);
    }
}

/* ------------------------------------------------------------------------- */
/* B5: an accessible length that cannot contain every row fails cleanly.     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 B5: an accessible length too short for every addressed row fails with camera_io/capture_failed",
          "[cvf-101][B5][negative]")
{
    SECTION("top-down, one byte short of the addressed extent")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 3u, 12);
        lock.fill(0u, 10u);
        lock.fill(2u, 200u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span() - 1u));
        check_camera_failure(result);
    }

    SECTION("top-down, only the first of three rows is reachable")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 3u, 12);
        lock.fill(0u, 10u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.row_bytes()));
        check_camera_failure(result);
    }

    SECTION("bottom-up, accessible length far too small for every row")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 3u, -12);
        lock.fill(0u, 10u);
        lock.fill(2u, 200u);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.row_bytes()));
        check_camera_failure(result);
    }
}

/* ------------------------------------------------------------------------- */
/* B6: dimension/pitch arithmetic overflow fails cleanly.                    */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 B6: dimension and pitch arithmetic that would overflow fails cleanly",
          "[cvf-101][B6][negative]")
{
    std::vector<std::uint8_t> tiny(64u, 0u);

    SECTION("height times pitch overflows 32-bit row arithmetic")
    {
        Locked2DBufferView locked;
        locked.scanline0 = tiny.data();
        locked.pitch = 4;
        locked.width = 4u;
        locked.height = 0xFFFFFFFFu;
        locked.format = NativePixelFormat::rgb24;
        locked.accessible_bytes = tiny.size();
        locked.accessible_bytes_known = false;
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(locked));
    }

    SECTION("width times bytes-per-pixel overflows for a packed row")
    {
        Locked2DBufferView locked;
        locked.scanline0 = tiny.data();
        locked.pitch = 4;
        locked.width = 0xFFFFFFF0u;
        locked.height = 2u;
        locked.format = NativePixelFormat::rgb24;
        locked.accessible_bytes = tiny.size();
        locked.accessible_bytes_known = true;
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(locked));
    }

    SECTION("height times a large positive pitch exceeds every addressable extent")
    {
        Locked2DBufferView locked;
        locked.scanline0 = tiny.data();
        locked.pitch = 0x7FFFFFFF;
        locked.width = 0xFFFFFFFFu;
        locked.height = 0xFFFFFFFFu;
        locked.format = NativePixelFormat::rgb24;
        locked.accessible_bytes = tiny.size();
        locked.accessible_bytes_known = false;
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(locked));
    }
}

/* ------------------------------------------------------------------------- */
/* Negative cases.                                                           */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 negative: invalid dimensions or a null base pointer fail without crashing",
          "[cvf-101][negative]")
{
    SECTION("null base pointer with a nonzero extent and nonzero dimensions")
    {
        Locked2DBufferView locked;
        locked.scanline0 = nullptr;
        locked.pitch = 12;
        locked.width = 4u;
        locked.height = 4u;
        locked.format = NativePixelFormat::rgb24;
        locked.accessible_bytes = 64u;
        locked.accessible_bytes_known = true;
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(locked));
    }

    SECTION("zero width with a nonzero height")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 0u, 4u, 0);
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span())));
    }

    SECTION("zero height with a nonzero width")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 0u, 12);
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(lock.view(true, 0u)));
    }
}

TEST_CASE("CVF-101 negative: a pitch magnitude smaller than one packed row fails",
          "[cvf-101][negative]")
{
    SECTION("positive pitch below the packed row size")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 3u, 11);
        lock.fill(0u, 10u);
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span())));
    }

    SECTION("negative pitch magnitude below the packed row size")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 3u, -11);
        lock.fill(0u, 10u);
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span())));
    }

    SECTION("zero pitch with more than one row")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 4u, 3u, 0);
        lock.fill(0u, 10u);
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span())));
    }
}

/* ========================================================================= */
/* Verify-phase requirement-derived edge cases (added 2026-09-18).           */
/* These probe brief behaviors B2/B3/B5, the boundary_cases, and the         */
/* negative_cases across every native format and against channel swaps.      */
/* ========================================================================= */

/* ------------------------------------------------------------------------- */
/* Every packed format must reject a sub-row pitch magnitude, both signs.    */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 verify: every packed format rejects a pitch magnitude smaller than one row",
          "[cvf-101][negative][verify]")
{
    struct FormatCase {
        NativePixelFormat format;
        std::uint32_t width;
        std::int32_t row_bytes;
    };
    const FormatCase cases[] = {
        {NativePixelFormat::rgb24, 4u, 12},
        {NativePixelFormat::rgb32, 4u, 16},
        {NativePixelFormat::argb32, 4u, 16},
        {NativePixelFormat::yuy2, 4u, 8},
        {NativePixelFormat::uyvy, 4u, 8},
    };

    for (const FormatCase& c : cases) {
        for (int sign = 0; sign < 2; ++sign) {
            const std::int32_t pitch = sign == 0 ? (c.row_bytes - 1) : -(c.row_bytes - 1);
            SyntheticLock lock(c.format, c.width, 3u, pitch);
            lock.fill(0u, 10u);
            INFO("format " << static_cast<int>(c.format) << " pitch " << pitch);
            check_camera_failure(uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span())));
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Negative pitch (bottom-up) for every packed and planar format.            */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 verify: negative pitch preserves orientation for every packed and planar format",
          "[cvf-101][B2][verify]")
{
    /* Packed formats: gray ramp 0 (top), 128 (middle), 255 (bottom), exact for
     * RGB whose per-channel value passes through unchanged. */
    const NativePixelFormat packed[] = {
        NativePixelFormat::rgb24,
        NativePixelFormat::rgb32,
        NativePixelFormat::argb32,
        NativePixelFormat::yuy2,
        NativePixelFormat::uyvy,
    };
    for (NativePixelFormat format : packed) {
        const std::uint32_t width = 4u;
        const std::uint32_t height = 3u;
        const std::int32_t pitch = -static_cast<std::int32_t>(packed_row_bytes(format, width));
        SyntheticLock lock(format, width, height, pitch);
        lock.fill(0u, 0u);
        lock.fill(1u, 128u);
        lock.fill(2u, 255u);

        INFO("packed format " << static_cast<int>(format));
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), width, height);

        if (format == NativePixelFormat::rgb24 || format == NativePixelFormat::rgb32 ||
            format == NativePixelFormat::argb32) {
            check_uniform_row(result.value(), 0, cv::Vec3b(0, 0, 0));
            check_uniform_row(result.value(), 1, cv::Vec3b(128, 128, 128));
            check_uniform_row(result.value(), 2, cv::Vec3b(255, 255, 255));
        } else {
            for (int row = 0; row < 3; ++row) {
                CHECK(row_is_neutral(result.value(), row));
            }
            CHECK(row_luma_proxy(result.value(), 0) < row_luma_proxy(result.value(), 1));
            CHECK(row_luma_proxy(result.value(), 1) < row_luma_proxy(result.value(), 2));
        }
    }

    /* Planar 4:2:0 formats are decoded from a top-down (positive-pitch) lock:
     * the change contract fixes U = V = 128 as neutral, so a fully neutral
     * planar buffer must yield equal B, G, and R channels and must preserve the
     * visual row order (dark luma first, bright luma last). */
    const NativePixelFormat planar[] = {
        NativePixelFormat::nv12,
        NativePixelFormat::i420,
        NativePixelFormat::yv12,
    };
    for (NativePixelFormat format : planar) {
        const std::uint32_t width = 4u;
        const std::uint32_t height = 4u;
        PlanarLock lock(format, width, height, static_cast<std::int32_t>(width));
        lock.fill(0u, 0u);
        lock.fill(2u, 128u);
        lock.fill(3u, 255u);

        INFO("planar format " << static_cast<int>(format));
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), width, height);
        CHECK(row_is_neutral(result.value(), 0));
        CHECK(row_is_neutral(result.value(), 3));
        CHECK(row_luma_proxy(result.value(), 0) < row_luma_proxy(result.value(), 3));
    }
}

/* ------------------------------------------------------------------------- */
/* Bottom-up planar locks must not read out of bounds. The frozen seam      */
/* header documents signed pitch generally, but the change contract's planar */
/* clause preserves the existing canonical 4:2:0 handling; this probe only   */
/* requires a clean outcome, not a particular one.                          */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 verify: a bottom-up planar lock never reads out of bounds",
          "[cvf-101][B6][verify]")
{
    const NativePixelFormat planar[] = {
        NativePixelFormat::nv12,
        NativePixelFormat::i420,
        NativePixelFormat::yv12,
    };
    for (NativePixelFormat format : planar) {
        PlanarLock lock(format, 4u, 4u, -4);
        lock.fill(0u, 0u);
        lock.fill(3u, 255u);

        INFO("planar format " << static_cast<int>(format));
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        if (result.has_value()) {
            /* If the seam accepts it, orientation must still be preserved. */
            check_frame_shape(result.value(), 4u, 4u);
            CHECK(row_luma_proxy(result.value(), 0) < row_luma_proxy(result.value(), 3));
        } else {
            /* Otherwise the documented clean failure, never a crash or a read. */
            check_camera_failure(result);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Distinct B,G,R channel order must not be swapped (rgb24/rgb32/argb32).    */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 verify: a channel-swapping decoder fails the distinct B,G,R identity assertion",
          "[cvf-101][B3][channel-order][verify]")
{
    /*
     * Distinct per-channel values whose B,G,R order differs from their R,G,B
     * order. Both an exact identity assertion and an explicit non-swap
     * assertion are made, so a decoder that reverses the low three bytes fails
     * even if it were to pass one of them by coincidence.
     */
    const cv::Vec3b exact(17, 99, 231); /* B=17, G=99, R=231 */
    const cv::Vec3b swapped(231, 99, 17); /* the channel-swap output */

    SECTION("rgb24 top-down")
    {
        SyntheticLock lock(NativePixelFormat::rgb24, 2u, 1u, 6);
        lock.fill_rgb24(0u, exact[0], exact[1], exact[2]);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_uniform_row(result.value(), 0, exact);
        CHECK(result.value().at<cv::Vec3b>(0, 0) != swapped);
    }

    SECTION("rgb32 bottom-up")
    {
        SyntheticLock lock(NativePixelFormat::rgb32, 2u, 1u, -8);
        lock.fill_bgr32(0u, exact[0], exact[1], exact[2], 0xFFu);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_uniform_row(result.value(), 0, exact);
        CHECK(result.value().at<cv::Vec3b>(0, 0) != swapped);
    }

    SECTION("argb32 top-down")
    {
        SyntheticLock lock(NativePixelFormat::argb32, 2u, 1u, 8);
        lock.fill_bgr32(0u, exact[0], exact[1], exact[2], 0x7Fu);
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_uniform_row(result.value(), 0, exact);
        CHECK(result.value().at<cv::Vec3b>(0, 0) != swapped);
    }
}

/* ------------------------------------------------------------------------- */
/* Accessible bounds: exactly-minimum accepted, one byte short rejected, for  */
/* every packed format.                                                      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 verify: accessible bounds boundary holds for every packed format",
          "[cvf-101][B5][boundary][verify]")
{
    struct FormatCase {
        NativePixelFormat format;
        std::uint32_t width;
        std::uint32_t height;
        std::int32_t pitch;
    };
    const FormatCase cases[] = {
        {NativePixelFormat::rgb24, 4u, 3u, 12},
        {NativePixelFormat::rgb32, 4u, 3u, 16},
        {NativePixelFormat::argb32, 4u, 3u, 16},
        {NativePixelFormat::yuy2, 4u, 3u, 8},
        {NativePixelFormat::uyvy, 4u, 3u, 8},
    };

    for (const FormatCase& c : cases) {
        SyntheticLock lock(c.format, c.width, c.height, c.pitch);
        lock.fill(0u, 10u);
        lock.fill(static_cast<std::uint32_t>(c.height - 1u), 200u);

        INFO("format " << static_cast<int>(c.format) << " minimum extent " << lock.span());
        const auto accepted = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span()));
        REQUIRE(accepted.has_value());
        check_frame_shape(accepted.value(), c.width, c.height);

        const auto rejected = uvc::decode_locked_buffer_to_bgr8(lock.view(true, lock.span() - 1u));
        check_camera_failure(rejected);
    }
}

/* ------------------------------------------------------------------------- */
/* Pitch == INT32_MIN must be handled without signed-overflow UB.            */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 verify: pitch == INT32_MIN is rejected without undefined behavior",
          "[cvf-101][B6][negative][verify]")
{
    std::vector<std::uint8_t> buffer(256u, 0x80u);

    SECTION("INT32_MIN negative pitch, known accessible length")
    {
        Locked2DBufferView locked;
        locked.scanline0 = buffer.data() + 128u;
        locked.pitch = std::numeric_limits<std::int32_t>::min();
        locked.width = 4u;
        locked.height = 2u;
        locked.format = NativePixelFormat::rgb24;
        locked.accessible_bytes = buffer.size();
        locked.accessible_bytes_known = true;
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(locked));
    }

    SECTION("INT32_MIN negative pitch, unknown accessible length")
    {
        Locked2DBufferView locked;
        locked.scanline0 = buffer.data() + 128u;
        locked.pitch = std::numeric_limits<std::int32_t>::min();
        locked.width = 4u;
        locked.height = 2u;
        locked.format = NativePixelFormat::rgb24;
        locked.accessible_bytes = 0u;
        locked.accessible_bytes_known = false;
        check_camera_failure(uvc::decode_locked_buffer_to_bgr8(locked));
    }
}

/* ------------------------------------------------------------------------- */
/* Width/height 1 edges for every format, both pitch signs where applicable. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 verify: width-1 and height-1 edges decode for every format that defines them",
          "[cvf-101][boundary][verify]")
{
    /* The 1x1 / width-1 luma edges apply to the byte-per-pixel packed formats.
     * 4:2:2 (yuy2/uyvy) needs an even width and 4:2:0 (nv12/i420/yv12) needs
     * even width and height, so their smallest valid frames are asserted via
     * their own minimum dimensions instead of a structurally impossible 1x1. */
    const NativePixelFormat byte_packed[] = {
        NativePixelFormat::rgb24,
        NativePixelFormat::rgb32,
        NativePixelFormat::argb32,
    };
    for (NativePixelFormat format : byte_packed) {
        const std::size_t row = packed_row_bytes(format, 1u);
        const std::int32_t pitch = static_cast<std::int32_t>(row);
        SyntheticLock lock(format, 1u, 1u, pitch);
        lock.fill(0u, 123u);

        INFO("packed 1x1 format " << static_cast<int>(format));
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 1u, 1u);
        CHECK(row_is_neutral(result.value(), 0));
    }

    /* Width 1 with height greater than 1, both pitch signs, byte-packed. */
    for (NativePixelFormat format : byte_packed) {
        const std::size_t row = packed_row_bytes(format, 1u);
        for (int sign = 0; sign < 2; ++sign) {
            const std::int32_t pitch = sign == 0 ? static_cast<std::int32_t>(row) : -static_cast<std::int32_t>(row);
            SyntheticLock lock(format, 1u, 3u, pitch);
            lock.fill(0u, 9u);
            lock.fill(2u, 240u);

            INFO("packed width-1 format " << static_cast<int>(format) << " pitch " << pitch);
            const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
            REQUIRE(result.has_value());
            check_frame_shape(result.value(), 1u, 3u);
            CHECK(row_is_neutral(result.value(), 0));
            CHECK(row_luma_proxy(result.value(), 0) < row_luma_proxy(result.value(), 2));
        }
    }

    /* Minimum valid 4:2:2 frame: width 2, height 1, both pitch signs, neutral. */
    const NativePixelFormat yuv422[] = {NativePixelFormat::yuy2, NativePixelFormat::uyvy};
    for (NativePixelFormat format : yuv422) {
        for (int sign = 0; sign < 2; ++sign) {
            const std::int32_t pitch = sign == 0 ? 4 : -4;
            SyntheticLock lock(format, 2u, 1u, pitch);
            lock.fill(0u, 55u);

            INFO("4:2:2 minimum format " << static_cast<int>(format) << " pitch " << pitch);
            const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
            REQUIRE(result.has_value());
            check_frame_shape(result.value(), 2u, 1u);
            CHECK(row_is_neutral(result.value(), 0));
        }
    }

    /* Minimum valid 4:2:0 frame: width 2, height 2, neutral planar. */
    const NativePixelFormat planar[] = {
        NativePixelFormat::nv12,
        NativePixelFormat::i420,
        NativePixelFormat::yv12,
    };
    for (NativePixelFormat format : planar) {
        PlanarLock lock(format, 2u, 2u, 2);
        lock.fill(0u, 123u);

        INFO("planar 2x2 minimum format " << static_cast<int>(format));
        const auto result = uvc::decode_locked_buffer_to_bgr8(lock.bounded_view());
        REQUIRE(result.has_value());
        check_frame_shape(result.value(), 2u, 2u);
        CHECK(row_is_neutral(result.value(), 0));
    }
}

/* ------------------------------------------------------------------------- */
/* Null scanline0 at the 1x1 edge must still fail without crashing.          */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-101 verify: a null scanline0 with 1x1 dimensions fails without crash",
          "[cvf-101][negative][verify]")
{
    Locked2DBufferView locked;
    locked.scanline0 = nullptr;
    locked.pitch = 3;
    locked.width = 1u;
    locked.height = 1u;
    locked.format = NativePixelFormat::rgb24;
    locked.accessible_bytes = 3u;
    locked.accessible_bytes_known = true;
    check_camera_failure(uvc::decode_locked_buffer_to_bgr8(locked));
}

}  // namespace cvf101

#endif /* defined(_WIN32) */

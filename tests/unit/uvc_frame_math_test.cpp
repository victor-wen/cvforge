/*
 * Developer-owned portable tests for the CVF-101 UVC locked-buffer helpers.
 *
 * The Windows-only seam decode_locked_buffer_to_bgr8() lives behind _WIN32, so
 * its pure arithmetic and color helpers were extracted into
 * src/camera/uvc_windows/uvc_frame_math.h. That header has no Windows API,
 * Media Foundation, or OpenCV dependency, so these tests cover the
 * overflow-safe row arithmetic and the full-range YUV conversion on the
 * portable Linux CI host.
 *
 * This file is intentionally not named cvf1*_*: the cvf1* independent Windows
 * component tests are owned and hash-pinned by the test owner.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>

#include "camera/uvc_windows/uvc_frame_math.h"

namespace {

namespace detail = cvforwin::camera::uvc::detail;

constexpr std::size_t k_size_max = std::numeric_limits<std::size_t>::max();

}  // namespace

TEST_CASE("CVF-101 helper: checked multiplication rejects size_t overflow", "[cvf-101][helper][math]")
{
    std::size_t result = 0;
    CHECK(detail::checked_mul(0u, k_size_max, result));
    CHECK(result == 0u);
    CHECK(detail::checked_mul(6u, 7u, result));
    CHECK(result == 42u);

    CHECK_FALSE(detail::checked_mul(2u, k_size_max, result));
    CHECK_FALSE(detail::checked_mul(k_size_max, k_size_max, result));
    /* The exact maximum that still fits is accepted. */
    CHECK(detail::checked_mul(k_size_max / 2u, 2u, result));
    CHECK(result == k_size_max - 1u);
}

TEST_CASE("CVF-101 helper: checked addition rejects size_t overflow", "[cvf-101][helper][math]")
{
    std::size_t result = 0;
    CHECK(detail::checked_add(0u, 0u, result));
    CHECK(result == 0u);
    CHECK(detail::checked_add(k_size_max - 1u, 1u, result));
    CHECK(result == k_size_max);

    CHECK_FALSE(detail::checked_add(1u, k_size_max, result));
    CHECK_FALSE(detail::checked_add(k_size_max, k_size_max, result));
}

TEST_CASE("CVF-101 helper: the pitch magnitude rejects INT32_MIN and mirrors the sign", "[cvf-101][helper][math]")
{
    std::size_t magnitude = 0;
    CHECK(detail::pitch_magnitude(0, magnitude));
    CHECK(magnitude == 0u);
    CHECK(detail::pitch_magnitude(18, magnitude));
    CHECK(magnitude == 18u);
    CHECK(detail::pitch_magnitude(-18, magnitude));
    CHECK(magnitude == 18u);

    /* INT32_MIN cannot be negated in int32, so it is rejected explicitly. */
    CHECK_FALSE(detail::pitch_magnitude(std::numeric_limits<std::int32_t>::min(), magnitude));
    CHECK(detail::pitch_magnitude(std::numeric_limits<std::int32_t>::max(), magnitude));
    CHECK(magnitude == static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()));
}

TEST_CASE("CVF-101 helper: packed region extent is (height-1)*|pitch| + row_bytes", "[cvf-101][helper][math]")
{
    std::size_t extent = 0;
    /* 4x3 rgb24, pitch 12: (3-1)*12 + 12 = 36. */
    CHECK(detail::packed_region_extent(12, 3u, 12u, extent));
    CHECK(extent == 36u);
    /* Same visual content, bottom-up memory: the magnitude is symmetric. */
    CHECK(detail::packed_region_extent(-12, 3u, 12u, extent));
    CHECK(extent == 36u);
    /* A padded row: (3-1)*20 + 15 = 55. */
    CHECK(detail::packed_region_extent(20, 3u, 15u, extent));
    CHECK(extent == 55u);
    /* A single row occupies exactly one packed row. */
    CHECK(detail::packed_region_extent(12, 1u, 12u, extent));
    CHECK(extent == 12u);
    /* A height of 1 ignores the pitch magnitude entirely. */
    CHECK(detail::packed_region_extent(4096, 1u, 3u, extent));
    CHECK(extent == 3u);

    /*
     * With a 64-bit size_t the product (height-1)*|pitch| (at most ~2^63) plus
     * row_bytes cannot overflow, so the overflow guard here is only reachable
     * with a 32-bit size_t. The guard itself is covered directly by the
     * checked_mul/checked_add cases above.
     */
    if constexpr (sizeof(std::size_t) < 8u) {
        CHECK_FALSE(detail::packed_region_extent(0x7FFFFFFF, 0xFFFFFFFFu, 3u, extent));
    }
}

TEST_CASE("CVF-101 helper: row region offset is measured from the lowest addressed byte",
          "[cvf-101][helper][math]")
{
    std::size_t offset = 0;

    /* Positive pitch: the region base is scanline0, so row r is at r*pitch. */
    CHECK(detail::packed_row_region_offset(0u, 12, 3u, offset));
    CHECK(offset == 0u);
    CHECK(detail::packed_row_region_offset(2u, 12, 3u, offset));
    CHECK(offset == 24u);

    /*
     * Negative pitch: scanline0 is the visual top row and lies at the highest
     * address, so visual row r is (height-1-r) rows above the region base.
     */
    CHECK(detail::packed_row_region_offset(0u, -12, 3u, offset));
    CHECK(offset == 24u);
    CHECK(detail::packed_row_region_offset(2u, -12, 3u, offset));
    CHECK(offset == 0u);
    /* Both pitch signs agree on the minimum extent at the last visual row. */
    CHECK(detail::packed_row_region_offset(2u, 12, 3u, offset));
    const std::size_t positive_last = offset;
    CHECK(detail::packed_row_region_offset(0u, -12, 3u, offset));
    CHECK(offset == positive_last);

    /*
     * The offset product at most reaches ~2^63, so overflow is unreachable with
     * a 64-bit size_t. The overflow guard is covered directly by the
     * checked_mul cases above.
     */
    if constexpr (sizeof(std::size_t) < 8u) {
        CHECK_FALSE(detail::packed_row_region_offset(0xFFFFFFFFu, 0x7FFFFFFF, 0xFFFFFFFFu, offset));
    }
}

TEST_CASE("CVF-101 helper: a row fits only when its end lies within the accessible length",
          "[cvf-101][helper][math]")
{
    /* 4x3 rgb24, pitch 12: rows end at 12, 24, 36. */
    CHECK(detail::packed_row_fits(0u, 12, 3u, 12u, 36u));
    CHECK(detail::packed_row_fits(1u, 12, 3u, 12u, 36u));
    CHECK(detail::packed_row_fits(2u, 12, 3u, 12u, 36u));
    CHECK_FALSE(detail::packed_row_fits(2u, 12, 3u, 12u, 35u));
    /* Row 1 ends exactly at byte 24, so 24 is the inclusive boundary. */
    CHECK(detail::packed_row_fits(1u, 12, 3u, 12u, 24u));
    CHECK_FALSE(detail::packed_row_fits(1u, 12, 3u, 12u, 23u));

    /*
     * Bottom-up memory: the visual top row is the last in memory, so its end is
     * the whole extent; the visual bottom row ends at one packed row.
     */
    CHECK(detail::packed_row_fits(0u, -12, 3u, 12u, 36u));
    CHECK(detail::packed_row_fits(2u, -12, 3u, 12u, 36u));
    CHECK_FALSE(detail::packed_row_fits(0u, -12, 3u, 12u, 35u));
}

TEST_CASE("CVF-101 helper: the lowest addressed byte follows the pitch sign", "[cvf-101][helper][math]")
{
    std::size_t lowest = 0;
    /* Positive/top-down: scanline0 is the lowest addressed byte. */
    CHECK(detail::lowest_addressed_address(1000u, 12, 3u, lowest));
    CHECK(lowest == 1000u);
    /*
     * Negative/bottom-up: scanline0 is the visual top row at the highest
     * address, so the lowest addressed byte is (height-1)*|pitch| below it.
     */
    CHECK(detail::lowest_addressed_address(1000u, -12, 3u, lowest));
    CHECK(lowest == 976u);
    /* A single row cannot descend, so both pitch signs agree. */
    CHECK(detail::lowest_addressed_address(1000u, -40, 1u, lowest));
    CHECK(lowest == 1000u);
    /* Zero height addresses no byte at all. */
    CHECK_FALSE(detail::lowest_addressed_address(1000u, 12, 0u, lowest));
    /* INT32_MIN has no representable magnitude. */
    CHECK_FALSE(detail::lowest_addressed_address(1000u, std::numeric_limits<std::int32_t>::min(), 2u, lowest));
    /* A descent below address zero is rejected instead of wrapping. */
    CHECK_FALSE(detail::lowest_addressed_address(10u, -12, 3u, lowest));
}

TEST_CASE("CVF-101 helper: the accessible extent is rebased onto the lowest addressed byte",
          "[cvf-101][helper][math]")
{
    std::size_t accessible = 0;
    /* Full-frame top-down: the lowest byte is the reported start. */
    CHECK(detail::rebase_accessible_extent(1000u, 1000u, 36u, accessible));
    CHECK(accessible == 36u);
    /*
     * The lock range starts below the scanned region: the extent is the
     * remaining length from the lowest addressed byte, not the raw length.
     */
    CHECK(detail::rebase_accessible_extent(1000u, 1100u, 1024u, accessible));
    CHECK(accessible == 924u);
    /* The lowest addressed byte is the last byte of the reported range. */
    CHECK(detail::rebase_accessible_extent(1000u, 1036u, 37u, accessible));
    CHECK(accessible == 1u);
    /* A range that ends at or below the lowest addressed byte is rejected. */
    CHECK_FALSE(detail::rebase_accessible_extent(1000u, 1036u, 36u, accessible));
    CHECK_FALSE(detail::rebase_accessible_extent(1000u, 999u, 36u, accessible));
    /* An empty reported range cannot bound anything. */
    CHECK_FALSE(detail::rebase_accessible_extent(1000u, 1000u, 0u, accessible));
    /* End-of-range overflow is rejected instead of wrapping. */
    CHECK_FALSE(detail::rebase_accessible_extent(k_size_max, k_size_max, 1u, accessible));
}

TEST_CASE("CVF-101 helper: a lock range yields the accessible extent from its lowest addressed byte",
          "[cvf-101][helper][math]")
{
    std::size_t accessible = 0;
    /*
     * Full-frame top-down rgb24 4x3, pitch 12, scanline0 == buffer_start: the
     * seam receives the whole 36-byte extent.
     */
    CHECK(detail::lock_accessible_extent(1000u, 12, 3u, 1000u, 36u, accessible));
    CHECK(accessible == 36u);
    /*
     * Bottom-up memory, pitch -12, 4x3: scanline0 is the visual top row at the
     * highest address and the lowest addressed byte sits 24 below it, so a lock
     * covering the whole frame from that lowest byte yields 36.
     */
    CHECK(detail::lock_accessible_extent(1024u, -12, 3u, 1000u, 36u, accessible));
    CHECK(accessible == 36u);
    /*
     * The finding's asymmetry: a lock that starts at scanline0 (not at the
     * lowest addressed byte) for negative pitch cannot contain the addressed
     * region, so it must be rejected rather than trusting its raw length.
     */
    CHECK_FALSE(detail::lock_accessible_extent(1024u, -12, 3u, 1024u, 12u, accessible));
    /* A range that stops short of the lowest addressed byte is rejected. */
    CHECK_FALSE(detail::lock_accessible_extent(1024u, -12, 3u, 1024u, 23u, accessible));
    CHECK_FALSE(detail::lock_accessible_extent(1024u, -12, 3u, 1001u, 23u, accessible));
    /* An empty range and a zero height are rejected. */
    CHECK_FALSE(detail::lock_accessible_extent(1000u, 12, 3u, 1000u, 0u, accessible));
    CHECK_FALSE(detail::lock_accessible_extent(1000u, 12, 0u, 1000u, 36u, accessible));
    /* Start-of-range overflow is rejected instead of wrapping. */
    CHECK_FALSE(detail::lock_accessible_extent(k_size_max, 12, 2u, k_size_max, 8u, accessible));
}

TEST_CASE("CVF-101 helper: clamp_to_byte saturates to the 8-bit range", "[cvf-101][helper][math]")
{
    CHECK(detail::clamp_to_byte(-1000) == 0u);
    CHECK(detail::clamp_to_byte(-1) == 0u);
    CHECK(detail::clamp_to_byte(0) == 0u);
    CHECK(detail::clamp_to_byte(128) == 128u);
    CHECK(detail::clamp_to_byte(255) == 255u);
    CHECK(detail::clamp_to_byte(256) == 255u);
    CHECK(detail::clamp_to_byte(100000) == 255u);
}

TEST_CASE("CVF-101 helper: full-range YUV maps neutral chroma to the exact luma value",
          "[cvf-101][helper][yuv]")
{
    /*
     * The seam's orientation/neutrality contract: U = V = 128 must map to
     * (Y, Y, Y) exactly. OpenCV's COLOR_YUV2BGR_* macros use the limited
     * (16..235) range and would return 130 for Y=128, so the seam owns its own
     * full-range conversion.
     */
    for (int luma = 0; luma <= 255; ++luma) {
        const detail::Bgr8 pixel = detail::yuv_to_bgr_full(static_cast<std::uint8_t>(luma), 128u, 128u);
        INFO("luma " << luma);
        CHECK(pixel.b == static_cast<std::uint8_t>(luma));
        CHECK(pixel.g == static_cast<std::uint8_t>(luma));
        CHECK(pixel.r == static_cast<std::uint8_t>(luma));
    }
}

TEST_CASE("CVF-101 helper: full-range YUV clamps the chroma extremes without wrapping",
          "[cvf-101][helper][yuv]")
{
    /* Y=128 with U=V=0 is green; U=V=255 is magenta. Exact channel identity is
     * what matters, not the BT.601 coefficient rounding. */
    const detail::Bgr8 green = detail::yuv_to_bgr_full(128u, 0u, 0u);
    CHECK(green.b == 0u);
    CHECK(green.r == 0u);
    CHECK(green.g > 200u);

    const detail::Bgr8 magenta = detail::yuv_to_bgr_full(128u, 255u, 255u);
    CHECK(magenta.b == 255u);
    CHECK(magenta.r == 255u);
    CHECK(magenta.g < 40u);

    /* Pure chroma swings must never wrap to the opposite channel. */
    const detail::Bgr8 blue = detail::yuv_to_bgr_full(128u, 255u, 0u);
    CHECK(blue.b > 200u);
    CHECK(blue.r == 0u);

    const detail::Bgr8 red = detail::yuv_to_bgr_full(128u, 0u, 255u);
    CHECK(red.r > 200u);
    CHECK(red.b == 0u);
}

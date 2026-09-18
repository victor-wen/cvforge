// CVF-105 independent black-box tests: canonicalize_hex4.
//
// Brief B1: vendor_id/product_id accept lower and upper hexadecimal and are
// observable as lowercase canonical four-hex values; invalid hex or wrong
// length is rejected with config_value_invalid.
//
// The only production surface used is the frozen declaration
// core::Result<std::string> cvforwin::recipes::canonicalize_hex4(std::string_view)
// from src/recipes/config.h. No production .cpp is read.

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

#include "cvf105_test.helpers.h"

#include "core/error.h"
#include "core/result.h"
#include "core/status.h"
#include "recipes/config.h"

namespace {

namespace core = cvforwin::core;
namespace recipes = cvforwin::recipes;

bool is_lower_hex4(std::string_view value)
{
    if (value.size() != 4u) {
        return false;
    }
    for (const char character : value) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        if (!digit && !lower) {
            return false;
        }
    }
    return true;
}

void expect_canonical(std::string_view input, std::string_view expected)
{
    const core::Result<std::string> result = recipes::canonicalize_hex4(input);
    INFO("input: [" << std::string(input) << "]");
    REQUIRE(result.has_value());
    CHECK(result.value() == expected);
    CHECK(result.value().size() == 4u);
    CHECK(is_lower_hex4(result.value()));
}

void expect_invalid(std::string_view input)
{
    const core::Result<std::string> result = recipes::canonicalize_hex4(input);
    INFO("rejected input: [" << std::string(input) << "]");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.failure().code == core::ErrorCode::config_value_invalid);
    CHECK(result.failure().status == core::status_for(core::ErrorCode::config_value_invalid));
    CHECK(result.failure().status == core::Status::config_error);
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* B1 acceptance: upper and lower hexadecimal both canonicalize to lowercase. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B1: lowercase hexadecimal is returned unchanged", "[cvf-105][B1][hex]")
{
    expect_canonical("0000", "0000");
    expect_canonical("1a2b", "1a2b");
    expect_canonical("0c3d", "0c3d");
    expect_canonical("9f8e", "9f8e");
    expect_canonical("abcd", "abcd");
    expect_canonical("ffff", "ffff");
}

TEST_CASE("CVF-105 B1: uppercase and mixed case hexadecimal canonicalize to lowercase",
          "[cvf-105][B1][hex]")
{
    expect_canonical("1A2B", "1a2b");
    expect_canonical("0C3D", "0c3d");
    expect_canonical("ABCD", "abcd");
    expect_canonical("FFFF", "ffff");
    expect_canonical("AbCd", "abcd");
    expect_canonical("aBcD", "abcd");
    expect_canonical("1a2B", "1a2b");
    expect_canonical("0A0a", "0a0a");
}

TEST_CASE("CVF-105 B1 boundary: every hexadecimal digit maps to its lowercase form",
          "[cvf-105][B1][hex][boundary]")
{
    constexpr char kLower[] = "0123456789abcdef";
    constexpr char kUpper[] = "0123456789ABCDEF";

    for (std::size_t index = 0; index < 16u; ++index) {
        const std::string lower4(4u, kLower[index]);
        const std::string upper4(4u, kUpper[index]);
        INFO("digit index: " << index);
        expect_canonical(lower4, lower4);
        expect_canonical(upper4, lower4);
    }
}

TEST_CASE("CVF-105 B1 boundary: the all-zero and all-f values are accepted",
          "[cvf-105][B1][hex][boundary]")
{
    expect_canonical("0000", "0000");
    expect_canonical("0000", "0000");
    expect_canonical("ffff", "ffff");
    expect_canonical("FFFF", "ffff");
}

/* ------------------------------------------------------------------------- */
/* B1 negative: wrong length, non-hex, empty.                                 */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B1 negative: the empty value is rejected", "[cvf-105][B1][hex][negative]")
{
    expect_invalid("");
}

TEST_CASE("CVF-105 B1 negative: every wrong length is rejected",
          "[cvf-105][B1][hex][negative][boundary]")
{
    expect_invalid("0");
    expect_invalid("ab");
    expect_invalid("1a2");
    expect_invalid("1a2b3");
    expect_invalid("1a2b3c");
}

TEST_CASE("CVF-105 B1 negative: non-hexadecimal characters are rejected",
          "[cvf-105][B1][hex][negative]")
{
    expect_invalid("1a2g");
    expect_invalid("1G2B");
    expect_invalid("ZZZZ");
    expect_invalid("12 4");
    expect_invalid(" 123");
    expect_invalid("123 ");
    expect_invalid("1a2b ");
    expect_invalid(" 1a2b");
    expect_invalid("1a2-");
    expect_invalid("-1a2");
    expect_invalid("0x1a");
    expect_invalid("0X1A");
    expect_invalid("1a2b\n");
    expect_invalid("\n1a2b");
    expect_invalid("+123");
    expect_invalid("12.3");

    std::string non_ascii;
    non_ascii.push_back(static_cast<char>(0xC3));
    non_ascii.push_back(static_cast<char>(0xA9));
    non_ascii += "12";
    expect_invalid(non_ascii);
}

TEST_CASE("CVF-105 B1 negative: an embedded NUL byte is not a valid hexadecimal digit",
          "[cvf-105][B1][hex][negative]")
{
    const std::string with_nul("1a\0b", 4u);
    expect_invalid(std::string_view(with_nul.data(), with_nul.size()));
}

/* ------------------------------------------------------------------------- */
/* B1 boundary: every one of the sixteen case combinations is accepted and    */
/* canonicalized identically (FR-024 is case-insensitive, not upper-only).    */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B1 boundary: all sixteen case combinations of a four-hex value canonicalize",
          "[cvf-105][B1][hex][boundary]")
{
    const std::string canonical = "abcd";
    for (unsigned mask = 0u; mask < 16u; ++mask) {
        std::string mixed = canonical;
        for (unsigned position = 0u; position < 4u; ++position) {
            if ((mask & (1u << position)) != 0u) {
                mixed[position] =
                    static_cast<char>(std::toupper(static_cast<unsigned char>(mixed[position])));
            }
        }
        INFO("case mask: " << mask << " input: [" << mixed << "]");
        expect_canonical(mixed, canonical);
    }
}

/* ------------------------------------------------------------------------- */
/* B1 negative: a 0x prefix, a five-hex value, and non-ASCII / odd bytes are   */
/* each rejected with config_value_invalid.                                    */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B1 negative: 0x-prefixed, five-character, and odd-byte values are rejected",
          "[cvf-105][B1][hex][negative][boundary]")
{
    // A 0x-prefixed four-hex value is six characters, never exactly four hex.
    expect_invalid("0x1a2b");
    expect_invalid("0X1a2b");
    expect_invalid("0xABCD");

    // Five all-hexadecimal characters still have the wrong length.
    expect_invalid("abcde");
    expect_invalid("12345");
    expect_invalid("ABCDE");

    // A lone UTF-8 continuation byte, a lone lead byte, and a complete
    // two-character multibyte sequence are odd byte sequences, not four ASCII
    // hexadecimal digits.
    std::string lone_continuation;
    lone_continuation.push_back(static_cast<char>(0x80));
    lone_continuation += "123";
    expect_invalid(lone_continuation);

    std::string lone_lead;
    lone_lead.push_back(static_cast<char>(0xC3));
    lone_lead += "123";
    expect_invalid(lone_lead);

    std::string multibyte_pair;
    multibyte_pair.push_back(static_cast<char>(0xC3));
    multibyte_pair.push_back(static_cast<char>(0xA9));
    multibyte_pair.push_back(static_cast<char>(0xC3));
    multibyte_pair.push_back(static_cast<char>(0xA9));
    expect_invalid(multibyte_pair);
}

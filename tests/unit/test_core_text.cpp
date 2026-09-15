/*
 * Developer unit tests for the bounded UTF-8 text helpers and caller-buffer
 * rules that back the public v1 ABI.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

#include "core/text.h"

using cvforwin::core::TextProblem;
using cvforwin::core::TextValidation;
using cvforwin::core::TextWriteResult;

TEST_CASE("validate_utf8 accepts well-formed sequences", "[core][text][utf8]")
{
    CHECK(cvforwin::core::validate_utf8("").ok);
    CHECK(cvforwin::core::validate_utf8("plain ascii").ok);
    CHECK(cvforwin::core::validate_utf8("caf\xC3\xA9").ok); /* U+00E9 */
    CHECK(cvforwin::core::validate_utf8("\xE2\x82\xAC 42").ok); /* U+20AC */
    CHECK(cvforwin::core::validate_utf8("\xF0\x9F\x98\x80").ok); /* U+1F600 */
    CHECK(cvforwin::core::validate_utf8("\xF4\x8F\xBF\xBF").ok); /* U+10FFFF */
}

TEST_CASE("validate_utf8 rejects malformed sequences", "[core][text][utf8]")
{
    CHECK_FALSE(cvforwin::core::validate_utf8("\x80").ok); /* lone continuation */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xC3").ok); /* truncated two-byte */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xE2\x82").ok); /* truncated three-byte */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xC0\x80").ok); /* overlong NUL */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xC1\xBF").ok); /* overlong */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xE0\x80\x80").ok); /* overlong three-byte */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xED\xA0\x80").ok); /* UTF-16 surrogate */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xF5\x80\x80\x80").ok); /* above U+10FFFF */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xF4\x90\x80\x80").ok); /* above U+10FFFF */
    CHECK_FALSE(cvforwin::core::validate_utf8("\xFF").ok); /* impossible lead byte */
    CHECK_FALSE(cvforwin::core::validate_utf8("bad\xC3\x28").ok); /* bad continuation */
}

TEST_CASE("utf8_safe_prefix never splits a code point", "[core][text][utf8]")
{
    const std::string two_byte = "caf\xC3\xA9"; /* "cafe" + U+00E9, 5 bytes */
    const std::string four_byte = "\xF0\x9F\x98\x80"; /* U+1F600, 4 bytes */
    const std::string ascii = "plain";

    SECTION("a limit on a code point boundary keeps the full prefix")
    {
        CHECK(cvforwin::core::utf8_safe_prefix(two_byte, 5u) == std::string_view(two_byte));
        CHECK(cvforwin::core::utf8_safe_prefix(two_byte, 3u) == std::string_view("caf"));
    }

    SECTION("a cut inside a two-byte sequence backs off to the boundary")
    {
        const std::string_view cut = cvforwin::core::utf8_safe_prefix(two_byte, 4u);
        CHECK(cut == std::string_view("caf"));
        CHECK(cvforwin::core::validate_utf8(cut).ok);
    }

    SECTION("a cut inside a four-byte sequence backs off to the previous boundary")
    {
        CHECK(cvforwin::core::utf8_safe_prefix(four_byte, 3u).empty());
        CHECK(cvforwin::core::utf8_safe_prefix(four_byte, 2u).empty());
        CHECK(cvforwin::core::utf8_safe_prefix(four_byte, 1u).empty());
        CHECK(cvforwin::core::validate_utf8(cvforwin::core::utf8_safe_prefix(four_byte, 3u)).ok);
    }

    SECTION("the limit bounds the result and zero is always safe")
    {
        CHECK(cvforwin::core::utf8_safe_prefix(ascii, 3u) == std::string_view("pla"));
        CHECK(cvforwin::core::utf8_safe_prefix(ascii, 0u).empty());
        CHECK(cvforwin::core::utf8_safe_prefix(two_byte, 100u) == std::string_view(two_byte));
        CHECK(cvforwin::core::validate_utf8(cvforwin::core::utf8_safe_prefix(two_byte, 4u)).ok);
    }
}

TEST_CASE("validate_text enforces byte limits and rejects embedded NUL", "[core][text]")
{
    const TextValidation empty = cvforwin::core::validate_text("", 8u);
    CHECK(empty.ok);

    const TextValidation fits = cvforwin::core::validate_text("12345678", 8u);
    CHECK(fits.ok);

    const TextValidation too_long = cvforwin::core::validate_text("123456789", 8u);
    CHECK_FALSE(too_long.ok);
    CHECK(too_long.problem == TextProblem::too_long);

    const std::string with_nul("ab\0cd", 5u);
    const TextValidation embedded = cvforwin::core::validate_text(with_nul, 8u);
    CHECK_FALSE(embedded.ok);
    CHECK(embedded.problem == TextProblem::embedded_nul);

    const TextValidation invalid = cvforwin::core::validate_text("\xC3\x28", 8u);
    CHECK_FALSE(invalid.ok);
    CHECK(invalid.problem == TextProblem::invalid_utf8);
}

TEST_CASE("write_text follows the v1 buffer rules", "[core][text]")
{
    SECTION("a buffer that fits reports content bytes and required including NUL")
    {
        char buffer[16] = {};
        const TextWriteResult written = cvforwin::core::write_text(buffer, 16u, "hello");
        CHECK(written.bytes_written == 5u);
        CHECK(written.bytes_required == 6u);
        CHECK(std::string_view(buffer) == "hello");
    }

    SECTION("a short buffer truncates and stays NUL-terminated")
    {
        char buffer[4] = {};
        const TextWriteResult written = cvforwin::core::write_text(buffer, 4u, "hello");
        CHECK(written.bytes_written == 3u);
        CHECK(written.bytes_required == 6u);
        CHECK(std::string_view(buffer) == "hel");
    }

    SECTION("capacity one writes only the terminator")
    {
        char buffer[1] = {'X'};
        const TextWriteResult written = cvforwin::core::write_text(buffer, 1u, "hello");
        CHECK(written.bytes_written == 0u);
        CHECK(written.bytes_required == 6u);
        CHECK(buffer[0] == '\0');
    }

    SECTION("a NULL buffer with capacity zero reports required without writing")
    {
        const TextWriteResult written = cvforwin::core::write_text(nullptr, 0u, "hello");
        CHECK(written.bytes_written == 0u);
        CHECK(written.bytes_required == 6u);
    }

    SECTION("an empty message still requires the trailing NUL")
    {
        char buffer[2] = {};
        const TextWriteResult written = cvforwin::core::write_text(buffer, 2u, "");
        CHECK(written.bytes_written == 0u);
        CHECK(written.bytes_required == 1u);
        CHECK(buffer[0] == '\0');
    }
}

TEST_CASE("is_absolute_path accepts POSIX and Windows absolute paths", "[core][text][path]")
{
    CHECK(cvforwin::core::is_absolute_path("/opt/cvforwin/config"));
    CHECK(cvforwin::core::is_absolute_path("C:\\cvforwin\\config"));
    CHECK(cvforwin::core::is_absolute_path("C:/cvforwin/config"));
    CHECK(cvforwin::core::is_absolute_path("\\\\server\\share\\config"));
    CHECK_FALSE(cvforwin::core::is_absolute_path("relative/config"));
    CHECK_FALSE(cvforwin::core::is_absolute_path(""));
    CHECK_FALSE(cvforwin::core::is_absolute_path("C:"));
    CHECK_FALSE(cvforwin::core::is_absolute_path("C:relative"));
}

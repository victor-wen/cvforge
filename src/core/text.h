/*
 * Bounded UTF-8 text and caller-buffer helpers.
 *
 * These helpers implement the frozen v1 textual buffer rules and the strict
 * UTF-8 acceptance used by the validation boundary:
 *   - bytes_written counts content bytes excluding the trailing NUL and the
 *     written buffer is NUL-terminated whenever capacity >= 1;
 *   - bytes_required reports the size needed including the trailing NUL, even
 *     when nothing could be written;
 *   - an embedded NUL, truncated sequence, overlong form, surrogate, or code
 *     point above U+10FFFF is rejected.
 */

#ifndef CVFORWIN_SRC_CORE_TEXT_H_
#define CVFORWIN_SRC_CORE_TEXT_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cvforwin::core {

enum class TextProblem {
    none,
    null_pointer_with_capacity,
    too_long,
    embedded_nul,
    invalid_utf8,
};

struct TextValidation {
    bool ok = true;
    TextProblem problem = TextProblem::none;
};

/* Validates a caller-provided text view against max_bytes and UTF-8 rules. */
TextValidation validate_text(std::string_view text, std::size_t max_bytes) noexcept;

/* Validates UTF-8 well-formedness of a byte sequence. */
TextValidation validate_utf8(std::string_view text) noexcept;

/*
 * Returns the longest prefix of text with at most max_bytes bytes that does not
 * split a UTF-8 sequence. The returned prefix is safe to emit as UTF-8 text
 * even when the input is valid UTF-8 and max_bytes lands inside a code point.
 */
std::string_view utf8_safe_prefix(std::string_view text, std::size_t max_bytes) noexcept;

struct TextWriteResult {
    std::uint32_t bytes_written = 0;
    std::uint32_t bytes_required = 0;
};

/*
 * Copies an ASCII-safe diagnostic into a caller buffer following the frozen
 * buffer rules. A null buffer is dereferenced only when capacity is nonzero.
 */
TextWriteResult write_text(char* buffer, std::uint32_t capacity, std::string_view text) noexcept;

/* True when the path is absolute on either POSIX or Windows semantics. */
bool is_absolute_path(std::string_view path) noexcept;

/* Maximum accepted UTF-8 byte length of a configuration or output root. */
inline constexpr std::size_t kMaxRootPathUtf8Bytes = 32767;

}  // namespace cvforwin::core

#endif /* CVFORWIN_SRC_CORE_TEXT_H_ */

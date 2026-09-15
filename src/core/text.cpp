#include "core/text.h"

#include <algorithm>
#include <cstring>

namespace cvforwin::core {
namespace {

std::uint32_t utf8_continuation_count(unsigned char lead) noexcept
{
    if (lead < 0x80u) {
        return 0u;
    }
    if (lead >= 0xC2u && lead <= 0xDFu) {
        return 1u;
    }
    if (lead >= 0xE0u && lead <= 0xEFu) {
        return 2u;
    }
    if (lead >= 0xF0u && lead <= 0xF4u) {
        return 3u;
    }
    return 4u; /* 0x80..0xC1 and 0xF5..0xFF can never start a sequence. */
}

bool is_continuation(unsigned char byte) noexcept
{
    return (byte & 0xC0u) == 0x80u;
}

}  // namespace

TextValidation validate_utf8(std::string_view text) noexcept
{
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        const std::uint32_t continuations = utf8_continuation_count(lead);
        if (continuations == 4u) {
            return {false, TextProblem::invalid_utf8};
        }
        if (index + continuations >= text.size()) {
            return {false, TextProblem::invalid_utf8};
        }
        for (std::uint32_t offset = 1u; offset <= continuations; ++offset) {
            if (!is_continuation(static_cast<unsigned char>(text[index + offset]))) {
                return {false, TextProblem::invalid_utf8};
            }
        }
        if (continuations == 1u && lead < 0xC2u) {
            return {false, TextProblem::invalid_utf8}; /* overlong two-byte form */
        }
        if (continuations == 2u) {
            const auto second = static_cast<unsigned char>(text[index + 1u]);
            if (lead == 0xE0u && second < 0xA0u) {
                return {false, TextProblem::invalid_utf8}; /* overlong */
            }
            if (lead == 0xEDu && second >= 0xA0u) {
                return {false, TextProblem::invalid_utf8}; /* surrogate */
            }
        }
        if (continuations == 3u) {
            const auto second = static_cast<unsigned char>(text[index + 1u]);
            if (lead == 0xF0u && second < 0x90u) {
                return {false, TextProblem::invalid_utf8}; /* overlong */
            }
            if (lead == 0xF4u && second > 0x8Fu) {
                return {false, TextProblem::invalid_utf8}; /* above U+10FFFF */
            }
        }
        index += static_cast<std::size_t>(continuations) + 1u;
    }
    return {};
}

TextValidation validate_text(std::string_view text, std::size_t max_bytes) noexcept
{
    if (text.find('\0') != std::string_view::npos) {
        return {false, TextProblem::embedded_nul};
    }
    if (max_bytes != 0u && text.size() > max_bytes) {
        return {false, TextProblem::too_long};
    }
    return validate_utf8(text);
}

std::string_view utf8_safe_prefix(std::string_view text, std::size_t max_bytes) noexcept
{
    if (text.size() <= max_bytes) {
        return text;
    }
    std::size_t length = max_bytes;
    /* Back off while the cut would leave a continuation byte first. */
    while (length > 0u && (static_cast<unsigned char>(text[length]) & 0xC0u) == 0x80u) {
        --length;
    }
    return text.substr(0u, length);
}

TextWriteResult write_text(char* buffer, std::uint32_t capacity, std::string_view text) noexcept
{
    TextWriteResult result;
    result.bytes_required = static_cast<std::uint32_t>(text.size()) + 1u;
    if (capacity == 0u || buffer == nullptr) {
        return result;
    }
    const std::size_t writable = static_cast<std::size_t>(capacity) - 1u;
    const std::size_t count = std::min<std::size_t>(text.size(), writable);
    if (count > 0u) {
        std::memcpy(buffer, text.data(), count);
    }
    buffer[count] = '\0';
    result.bytes_written = static_cast<std::uint32_t>(count);
    return result;
}

bool is_absolute_path(std::string_view path) noexcept
{
    if (path.empty()) {
        return false;
    }
    if (path.front() == '/') {
        return true;
    }
    const char first = path.front();
    const bool drive_letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (drive_letter && path.size() >= 3u && path[1] == ':') {
        return path[2] == '/' || path[2] == '\\';
    }
    return path.size() >= 2u && path[0] == '\\' && path[1] == '\\';
}

}  // namespace cvforwin::core

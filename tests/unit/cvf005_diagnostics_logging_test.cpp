// CVF-005 independent black-box tests: level filtering, synchronous C callback
// delivery and truncation, rolling file sink bounds, and warning containment
// (brief B2, B3, B4, B6).

#include "cvf005_test_support.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace cvf005;

TEST_CASE("CVF-005 B2: entries below the configured level are dropped without callback or file "
          "growth",
          "[cvf-005][B2][diagnostics]")
{
    TempDir base("diag_filter");
    const auto log_dir = base.path() / "logs";
    CallbackRecord record;
    std::unique_ptr<diag::Diagnostics> diagnostics =
        create_diagnostics(log_dir, core::LogLevel::warn, record);
    const auto log_file = log_dir / "cvforwin.log";
    const auto size_after_create = size_of_file(log_file);

    diagnostics->log(core::LogLevel::trace, "cvf005 hidden trace");
    diagnostics->log(core::LogLevel::debug, "cvf005 hidden debug");
    diagnostics->log(core::LogLevel::info, "cvf005 hidden info");

    CHECK(record.calls.load() == 0u);
    CHECK(size_of_file(log_file) == size_after_create);

    diagnostics->log(core::LogLevel::warn, "cvf005 visible warn");
    diagnostics->log(core::LogLevel::error, "cvf005 visible error");
    diagnostics->log(core::LogLevel::critical, "cvf005 visible critical");

    CHECK(record.calls.load() == 3u);
    CHECK(record.levels == std::vector<std::uint32_t>({3u, 4u, 5u}));

    diagnostics.reset();  // flush and close the sink before reading it back

    const std::string log = read_bytes(log_file);
    CHECK(log.find("cvf005 visible warn") != std::string::npos);
    CHECK(log.find("cvf005 visible critical") != std::string::npos);
    CHECK(log.find("cvf005 hidden trace") == std::string::npos);
    CHECK(log.find("cvf005 hidden debug") == std::string::npos);
    CHECK(log.find("cvf005 hidden info") == std::string::npos);
    CHECK(size_of_file(log_file) > size_after_create);
}

TEST_CASE("CVF-005 B2 boundary: a trace level delivers every entry",
          "[cvf-005][B2][diagnostics][boundary]")
{
    TempDir base("diag_filter_trace");
    const auto log_dir = base.path() / "logs";
    CallbackRecord record;
    std::unique_ptr<diag::Diagnostics> diagnostics =
        create_diagnostics(log_dir, core::LogLevel::trace, record);

    diagnostics->log(core::LogLevel::trace, "cvf005 level trace");
    diagnostics->log(core::LogLevel::debug, "cvf005 level debug");
    diagnostics->log(core::LogLevel::info, "cvf005 level info");
    diagnostics->log(core::LogLevel::warn, "cvf005 level warn");
    diagnostics->log(core::LogLevel::error, "cvf005 level error");
    diagnostics->log(core::LogLevel::critical, "cvf005 level critical");

    CHECK(record.calls.load() == 6u);
    CHECK(record.levels == std::vector<std::uint32_t>({0u, 1u, 2u, 3u, 4u, 5u}));
}

TEST_CASE("CVF-005 B3: callback delivery is synchronous, exact, numeric, and NUL-terminated",
          "[cvf-005][B3][diagnostics]")
{
    TempDir base("diag_callback");
    const auto log_dir = base.path() / "logs";
    CallbackRecord record;
    std::unique_ptr<diag::Diagnostics> diagnostics =
        create_diagnostics(log_dir, core::LogLevel::trace, record);

    diagnostics->log(core::LogLevel::info, "cvf005 exact message");

    // Synchronous means observable before any flush/destruction happens.
    CHECK(record.calls.load() == 1u);
    CHECK(record.last_level == 2u);
    CHECK(record.last_message == "cvf005 exact message");
    CHECK(record.last_length == std::string_view("cvf005 exact message").size());

    diagnostics->log(core::LogLevel::trace, "");

    CHECK(record.calls.load() == 2u);
    CHECK(record.messages.at(1).empty());
    CHECK(record.levels.at(1) == 0u);
}

TEST_CASE("CVF-005 B3 boundary: long messages are truncated to a UTF-8-safe prefix of at most "
          "4096 bytes",
          "[cvf-005][B3][diagnostics][boundary]")
{
    TempDir base("diag_truncate");
    const auto log_dir = base.path() / "logs";
    CallbackRecord record;
    std::unique_ptr<diag::Diagnostics> diagnostics =
        create_diagnostics(log_dir, core::LogLevel::trace, record);

    SECTION("exactly 4096 bytes is delivered unchanged")
    {
        const std::string message(4096u, 'a');
        diagnostics->log(core::LogLevel::info, message);

        REQUIRE(record.calls.load() == 1u);
        CHECK(record.last_message == message);
        CHECK(record.last_message.size() == 4096u);
    }

    SECTION("4097 bytes collapses to the 4096-byte prefix")
    {
        std::string message(4097u, 'b');
        message.back() = 'z';
        diagnostics->log(core::LogLevel::info, message);

        REQUIRE(record.calls.load() == 1u);
        CHECK(record.last_message.size() == 4096u);
        CHECK(record.last_message == message.substr(0u, 4096u));
    }

    SECTION("a code point split by the limit backs off to the previous boundary")
    {
        std::string message(4095u, 'c');
        message += "\xF0\x9F\x98\x80";  // U+1F600, four bytes; total length 4099
        diagnostics->log(core::LogLevel::info, message);

        REQUIRE(record.calls.load() == 1u);
        CHECK(record.last_message.size() <= 4096u);
        CHECK(record.last_message == message.substr(0u, 4095u));
        CHECK(valid_utf8(record.last_message));
    }

    SECTION("a short multi-byte message is delivered unchanged")
    {
        const std::string message = "cvf005 caf\xC3\xA9 \xF0\x9F\x98\x80";
        diagnostics->log(core::LogLevel::info, message);

        REQUIRE(record.calls.load() == 1u);
        CHECK(record.last_message == message);
        CHECK(valid_utf8(record.last_message));
    }
}

TEST_CASE("CVF-005 B4: written entries appear in cvforwin.log", "[cvf-005][B4][diagnostics]")
{
    TempDir base("diag_sink");
    const auto log_dir = base.path() / "logs";
    CallbackRecord record;
    std::unique_ptr<diag::Diagnostics> diagnostics =
        create_diagnostics(log_dir, core::LogLevel::info, record);

    diagnostics->log(core::LogLevel::error, "cvf005 sink probe");
    diagnostics.reset();

    const std::string log = read_bytes(log_dir / "cvforwin.log");
    CHECK(log.find("cvf005 sink probe") != std::string::npos);
}

TEST_CASE("CVF-005 B4: rolling files stay within max_files", "[cvf-005][B4][diagnostics][boundary]")
{
    SECTION("1024-byte files with two files allowed")
    {
        TempDir base("diag_rotate_two");
        const auto log_dir = base.path() / "logs";
        CallbackRecord record;
        std::unique_ptr<diag::Diagnostics> diagnostics =
            create_diagnostics(log_dir, core::LogLevel::info, record, 1024u, 2u);

        const std::string payload(96u, '.');
        for (int index = 0; index < 40; ++index) {
            diagnostics->log(core::LogLevel::error,
                             "cvf005 rotation entry " + std::to_string(index) + payload);
        }
        diagnostics.reset();

        const std::size_t log_files = count_files_with_prefix(log_dir, "cvforwin");
        INFO("rotating log files: " << log_files);
        CHECK(log_files <= 2u);
        CHECK(log_files >= 1u);
        CHECK(std::filesystem::is_regular_file(log_dir / "cvforwin.log"));
        const std::string log = read_bytes(log_dir / "cvforwin.log");
        CHECK(log.find("cvf005 rotation entry 39") != std::string::npos);
    }

    SECTION("one file allowed")
    {
        TempDir base("diag_rotate_one");
        const auto log_dir = base.path() / "logs";
        CallbackRecord record;
        std::unique_ptr<diag::Diagnostics> diagnostics =
            create_diagnostics(log_dir, core::LogLevel::info, record, 1024u, 1u);

        const std::string payload(96u, '.');
        for (int index = 0; index < 40; ++index) {
            diagnostics->log(core::LogLevel::error,
                             "cvf005 rotation entry " + std::to_string(index) + payload);
        }
        diagnostics.reset();

        const std::size_t log_files = count_files_with_prefix(log_dir, "cvforwin");
        INFO("rotating log files: " << log_files);
        CHECK(log_files <= 1u);
        CHECK(log_files >= 1u);
        CHECK(std::filesystem::is_regular_file(log_dir / "cvforwin.log"));
    }
}

TEST_CASE("CVF-005 B6: a throwing callback is contained and sets warning_log_sink_failed",
          "[cvf-005][B6][diagnostics][negative]")
{
    TempDir base("diag_throwing");
    const auto log_dir = base.path() / "logs";
    CallbackRecord record;

    auto created = diag::Diagnostics::create(diagnostics_config(core::LogLevel::info, log_dir),
                                             throwing_binding(record));
    REQUIRE(created.has_value());
    std::unique_ptr<diag::Diagnostics> diagnostics = std::move(created.value());
    CHECK(warning_bits(diagnostics->warnings()) == 0u);

    CHECK_NOTHROW(diagnostics->log(core::LogLevel::info, "cvf005 throwing probe"));

    CHECK(record.calls.load() == 1u);
    CHECK(has_warning(diagnostics->warnings(), core::WarningFlags::warning_log_sink_failed));

    // The flag is sticky until explicitly cleared and is set again by the next
    // contained callback failure.
    CHECK_NOTHROW(diagnostics->log(core::LogLevel::warn, "cvf005 throwing probe two"));
    CHECK(record.calls.load() == 2u);
    CHECK(has_warning(diagnostics->warnings(), core::WarningFlags::warning_log_sink_failed));

    diagnostics->clear_warnings();
    CHECK(warning_bits(diagnostics->warnings()) == 0u);

    CHECK_NOTHROW(diagnostics->log(core::LogLevel::error, "cvf005 throwing probe three"));
    CHECK(record.calls.load() == 3u);
    CHECK(has_warning(diagnostics->warnings(), core::WarningFlags::warning_log_sink_failed));

    // A dropped entry never reaches the throwing callback, so a cleared flag
    // stays clear.
    diagnostics->clear_warnings();
    CHECK(warning_bits(diagnostics->warnings()) == 0u);
    CHECK_NOTHROW(diagnostics->log(core::LogLevel::trace, "cvf005 dropped probe"));
    CHECK(record.calls.load() == 3u);
    CHECK(warning_bits(diagnostics->warnings()) == 0u);
}

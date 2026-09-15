// CVF-005 independent black-box tests: Diagnostics::create validation, managed
// log directory creation, warning-flag values, and degraded-sink containment
// (brief B1, B5, B6; interface_reference warnings).

#include "cvf005_test_support.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

using namespace cvf005;

TEST_CASE("CVF-005 warning flags keep the frozen bit values", "[cvf-005][warning][contract]")
{
    CHECK(static_cast<std::uint32_t>(core::WarningFlags::warning_none) == 0u);
    CHECK(static_cast<std::uint32_t>(core::WarningFlags::warning_log_sink_failed) == 1u);
    CHECK(static_cast<std::uint32_t>(core::WarningFlags::warning_image_save_failed) == 2u);
}

TEST_CASE("CVF-005 B1 negative: a relative log_dir is rejected as path_not_absolute",
          "[cvf-005][B1][diagnostics][negative]")
{
    CallbackRecord record;
    auto created = diag::Diagnostics::create(
        diagnostics_config(core::LogLevel::info, std::filesystem::path{"cvf005-relative-logs"}),
        recording_binding(record));

    check_failure(created, core::Status::invalid_argument, core::ErrorCode::path_not_absolute);
}

TEST_CASE("CVF-005 B1 negative: invalid size bounds report diagnostics_invalid_config before side "
          "effects",
          "[cvf-005][B1][diagnostics][negative]")
{
    const auto expect_rejected = [](std::uint64_t max_file_bytes, std::uint32_t max_files) {
        TempDir base("diag_bounds");
        const auto log_dir = base.path() / "logs";
        CallbackRecord record;

        auto created = diag::Diagnostics::create(
            diagnostics_config(core::LogLevel::info, log_dir, max_file_bytes, max_files),
            recording_binding(record));

        check_failure(created, core::Status::invalid_argument,
                      core::ErrorCode::diagnostics_invalid_config);
        CHECK_FALSE(std::filesystem::exists(log_dir));
    };

    SECTION("max_file_bytes zero")
    {
        expect_rejected(0u, 5u);
    }
    SECTION("max_file_bytes one below the minimum")
    {
        expect_rejected(1023u, 5u);
    }
    SECTION("max_files zero")
    {
        expect_rejected(10485760u, 0u);
    }
}

TEST_CASE("CVF-005 B1 boundary: exactly 1024 bytes and one file are accepted",
          "[cvf-005][B1][diagnostics][boundary]")
{
    TempDir base("diag_min_bounds");
    const auto log_dir = base.path() / "logs";
    CallbackRecord record;

    auto created = diag::Diagnostics::create(
        diagnostics_config(core::LogLevel::info, log_dir, 1024u, 1u), recording_binding(record));

    REQUIRE(created.has_value());
    CHECK(std::filesystem::is_directory(log_dir));
    CHECK(std::filesystem::is_regular_file(log_dir / "cvforwin.log"));
}

TEST_CASE("CVF-005 B1: a valid config creates log_dir and cvforwin.log without warnings",
          "[cvf-005][B1][diagnostics]")
{
    TempDir base("diag_create");
    const auto log_dir = base.path() / "logs";
    CallbackRecord record;

    std::unique_ptr<diag::Diagnostics> diagnostics = create_diagnostics(
        log_dir, core::LogLevel::info, record);

    CHECK(std::filesystem::is_directory(log_dir));
    CHECK(std::filesystem::is_regular_file(log_dir / "cvforwin.log"));
    const std::filesystem::path reported{diagnostics->log_file_path()};
    CHECK(reported == log_dir / "cvforwin.log");
    CHECK(warning_bits(diagnostics->warnings()) == 0u);
}

TEST_CASE("CVF-005 B5: an uncreatable log directory degrades to a warning and keeps working",
          "[cvf-005][B5][diagnostics][negative]")
{
    TempDir base("diag_degraded");
    const auto blocker = base.path() / "blocker";
    write_bytes(blocker, 16u);
    const auto log_dir = blocker / "logs";
    CallbackRecord record;

    auto created = diag::Diagnostics::create(
        diagnostics_config(core::LogLevel::info, log_dir), recording_binding(record));

    REQUIRE(created.has_value());
    std::unique_ptr<diag::Diagnostics> diagnostics = std::move(created.value());

    CHECK(has_warning(diagnostics->warnings(), core::WarningFlags::warning_log_sink_failed));
    CHECK_FALSE(std::filesystem::exists(log_dir));

    CHECK_NOTHROW(diagnostics->log(core::LogLevel::info, "cvf005 degraded probe"));
    CHECK_NOTHROW(diagnostics->log(core::LogLevel::error, "cvf005 degraded probe error"));

    CHECK(record.calls.load() == 2u);
    CHECK(record.messages.at(0) == "cvf005 degraded probe");
    CHECK(record.messages.at(1) == "cvf005 degraded probe error");
    CHECK(record.levels.at(1) == 4u);
}

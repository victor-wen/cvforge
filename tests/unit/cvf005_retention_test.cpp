// CVF-005 independent black-box tests: age and size retention, symlink safety,
// and the non-existent-root failure (brief B11, B12, B13, B14).

#include "cvf005_test_support.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

using namespace cvf005;

namespace {

constexpr std::uint64_t kHugeTotal = std::uint64_t{1} << 40;
constexpr std::chrono::seconds kHugeAge{31536000};  // 365 days

}  // namespace

TEST_CASE("CVF-005 B11: retention deletes files older than max_age and reports the count",
          "[cvf-005][B11][retention]")
{
    TempDir base("retention_age");
    const auto root = base.path() / "captures";
    REQUIRE(std::filesystem::create_directories(root));

    const auto old_1 = root / "20240101T000000_recipe_request_1.png";
    const auto old_2 = root / "20240102T000000_recipe_request_2.png";
    const auto old_3 = root / "20240103T000000_recipe_request_3.png";
    const auto fresh = root / "20240104T000000_recipe_request_4.png";
    write_bytes(old_1, 100u);
    write_bytes(old_2, 100u);
    write_bytes(old_3, 100u);
    write_bytes(fresh, 100u);
    set_age(old_1, std::chrono::hours{3});
    set_age(old_2, std::chrono::hours{2});
    set_age(old_3, std::chrono::hours{1});
    set_age(fresh, std::chrono::seconds{0});

    auto deleted = art::CaptureStore::enforce_retention(root, std::chrono::seconds{60}, kHugeTotal);

    REQUIRE(deleted.has_value());
    check_integer(deleted.value(), 3, "deleted count");
    CHECK_FALSE(std::filesystem::exists(old_1));
    CHECK_FALSE(std::filesystem::exists(old_2));
    CHECK_FALSE(std::filesystem::exists(old_3));
    CHECK(std::filesystem::is_regular_file(fresh));
}

TEST_CASE("CVF-005 B11 boundary: an age inside max_age is kept and an age outside it is deleted",
          "[cvf-005][B11][retention][boundary]")
{
    // An instant exactly equal to the boundary is not representable without
    // controlling the implementation clock, so the inclusive "not expired"
    // direction is probed one full minute inside the boundary.
    SECTION("one minute inside a two-minute window is kept")
    {
        TempDir base("retention_age_in");
        const auto root = base.path() / "captures";
        REQUIRE(std::filesystem::create_directories(root));
        const auto file = root / "20240101T000000_recipe_request_1.png";
        write_bytes(file, 100u);
        set_age(file, std::chrono::seconds{60});

        auto deleted =
            art::CaptureStore::enforce_retention(root, std::chrono::seconds{120}, kHugeTotal);

        REQUIRE(deleted.has_value());
        check_integer(deleted.value(), 0, "deleted count");
        CHECK(std::filesystem::is_regular_file(file));
    }

    SECTION("one minute outside a two-minute window is deleted")
    {
        TempDir base("retention_age_out");
        const auto root = base.path() / "captures";
        REQUIRE(std::filesystem::create_directories(root));
        const auto file = root / "20240101T000000_recipe_request_1.png";
        write_bytes(file, 100u);
        set_age(file, std::chrono::seconds{180});

        auto deleted =
            art::CaptureStore::enforce_retention(root, std::chrono::seconds{120}, kHugeTotal);

        REQUIRE(deleted.has_value());
        check_integer(deleted.value(), 1, "deleted count");
        CHECK_FALSE(std::filesystem::exists(file));
    }
}

TEST_CASE("CVF-005 B12: retention deletes the oldest files until the total fits",
          "[cvf-005][B12][retention]")
{
    TempDir base("retention_size");
    const auto root = base.path() / "captures";
    REQUIRE(std::filesystem::create_directories(root));

    const auto oldest = root / "20240101T000000_recipe_request_1.png";
    const auto middle = root / "20240102T000000_recipe_request_2.png";
    const auto newest = root / "20240103T000000_recipe_request_3.png";
    write_bytes(oldest, 100u);
    write_bytes(middle, 100u);
    write_bytes(newest, 100u);
    set_age(oldest, std::chrono::hours{3});
    set_age(middle, std::chrono::hours{2});
    set_age(newest, std::chrono::hours{1});

    auto deleted = art::CaptureStore::enforce_retention(root, kHugeAge, 150u);

    REQUIRE(deleted.has_value());
    check_integer(deleted.value(), 2, "deleted count");
    CHECK_FALSE(std::filesystem::exists(oldest));
    CHECK_FALSE(std::filesystem::exists(middle));
    CHECK(std::filesystem::is_regular_file(newest));
    CHECK(size_of_file(newest) == 100u);
}

TEST_CASE("CVF-005 B12 boundary: a limit exactly equal to the total deletes nothing",
          "[cvf-005][B12][retention][boundary]")
{
    TempDir base("retention_size_equal");
    const auto root = base.path() / "captures";
    REQUIRE(std::filesystem::create_directories(root));

    const auto oldest = root / "20240101T000000_recipe_request_1.png";
    const auto middle = root / "20240102T000000_recipe_request_2.png";
    const auto newest = root / "20240103T000000_recipe_request_3.png";
    write_bytes(oldest, 100u);
    write_bytes(middle, 100u);
    write_bytes(newest, 100u);
    set_age(oldest, std::chrono::hours{3});
    set_age(middle, std::chrono::hours{2});
    set_age(newest, std::chrono::hours{1});

    auto deleted = art::CaptureStore::enforce_retention(root, kHugeAge, 300u);

    REQUIRE(deleted.has_value());
    check_integer(deleted.value(), 0, "deleted count");
    CHECK(std::filesystem::is_regular_file(oldest));
    CHECK(std::filesystem::is_regular_file(middle));
    CHECK(std::filesystem::is_regular_file(newest));
}

#ifndef _WIN32
TEST_CASE("CVF-005 B13: retention never follows or deletes symlinks",
          "[cvf-005][B13][retention][negative]")
{
    TempDir base("retention_links");
    const auto root = base.path() / "captures";
    const auto outside = base.path() / "outside";
    REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::create_directories(outside));

    const auto old_regular = root / "20240102T000000_recipe_request_10.png";
    write_bytes(old_regular, 100u);
    set_age(old_regular, std::chrono::hours{5});

    SECTION("an old target behind a fresh symlink is not reached")
    {
        const auto outside_file = outside / "outside_target_a.bin";
        write_bytes(outside_file, 64u);
        set_age(outside_file, std::chrono::hours{5});

        const auto link = root / "20240101T000000_recipe_request_9.png";
        std::error_code error;
        std::filesystem::create_symlink(outside_file, link, error);
        REQUIRE_FALSE(error);
        REQUIRE(std::filesystem::is_symlink(link));

        auto deleted =
            art::CaptureStore::enforce_retention(root, std::chrono::seconds{60}, kHugeTotal);

        REQUIRE(deleted.has_value());
        check_integer(deleted.value(), 1, "deleted count (only the old regular file)");
        CHECK(std::filesystem::is_symlink(link));
        CHECK(std::filesystem::read_symlink(link) == outside_file);
        CHECK(std::filesystem::is_regular_file(outside_file));
        CHECK(size_of_file(outside_file) == 64u);
        CHECK_FALSE(std::filesystem::exists(old_regular));
    }

    SECTION("an old symlink itself is not deleted")
    {
        const auto outside_file = outside / "outside_target_b.bin";
        write_bytes(outside_file, 64u);

        const auto link = root / "20240101T000000_recipe_request_8.png";
        std::error_code error;
        std::filesystem::create_symlink(outside_file, link, error);
        REQUIRE_FALSE(error);
        const bool link_is_old = set_symlink_age(link, std::chrono::hours{5});
        INFO("symlink no-follow timestamp control: " << link_is_old);

        auto deleted =
            art::CaptureStore::enforce_retention(root, std::chrono::seconds{60}, kHugeTotal);

        REQUIRE(deleted.has_value());
        check_integer(deleted.value(), 1, "deleted count (only the old regular file)");
        CHECK(std::filesystem::is_symlink(link));
        CHECK(std::filesystem::is_regular_file(outside_file));
        CHECK(size_of_file(outside_file) == 64u);
        CHECK_FALSE(std::filesystem::exists(old_regular));
    }
}

TEST_CASE("CVF-005 B13: a directory symlink is not walked into the outside tree",
          "[cvf-005][B13][retention][negative]")
{
    TempDir base("retention_dir_link");
    const auto root = base.path() / "captures";
    const auto outside = base.path() / "outside_tree";
    REQUIRE(std::filesystem::create_directories(root));
    REQUIRE(std::filesystem::create_directories(outside));

    for (int index = 1; index <= 3; ++index) {
        write_bytes(outside / ("outside_" + std::to_string(index) + ".png"), 100u);
    }

    const auto link = root / "cache";
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, link, error);
    REQUIRE_FALSE(error);
    REQUIRE(std::filesystem::is_symlink(link));

    const auto in_root = root / "20240101T000000_recipe_request_1.png";
    write_bytes(in_root, 100u);
    set_age(in_root, std::chrono::seconds{0});

    auto deleted = art::CaptureStore::enforce_retention(root, kHugeAge, 100u);

    REQUIRE(deleted.has_value());
    check_integer(deleted.value(), 0, "deleted count");
    CHECK(std::filesystem::is_regular_file(in_root));
    CHECK(std::filesystem::is_symlink(link));
    CHECK(count_files_with_extension(outside, ".png") == 3u);
}
#endif

TEST_CASE("CVF-005 B14 negative: retention on a non-existent root reports retention_error",
          "[cvf-005][B14][retention][negative]")
{
    TempDir base("retention_missing");
    const auto missing = base.path() / "no_such_captures";
    REQUIRE_FALSE(std::filesystem::exists(missing));

    auto deleted =
        art::CaptureStore::enforce_retention(missing, std::chrono::seconds{60}, kHugeTotal);

    check_failure(deleted, core::Status::internal_error, core::ErrorCode::retention_error);
}

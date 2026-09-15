/*
 * Developer unit tests for the CVF-005 diagnostics and managed-capture
 * additions. These complement the independent cvf005_* suites and pin paths
 * that suite does not exercise directly: the image_write_error mapping, the
 * non-recursive retention scan, the degraded log-path contract, and root
 * normalization for a trailing separator.
 */

#include <catch2/catch_test_macros.hpp>

#include "artifacts/capture_store.h"
#include "diagnostics/diagnostics.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <opencv2/core.hpp>

namespace {

namespace core = cvforwin::core;
namespace diag = cvforwin::diagnostics;
namespace art = cvforwin::artifacts;

using namespace std::chrono_literals;

class TempTree {
public:
    explicit TempTree(std::string_view tag)
    {
        static std::atomic<std::uint64_t> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("cvf005_dev_" + std::string(tag) + "_" + std::to_string(counter.fetch_add(1)));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        REQUIRE_FALSE(error);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    ~TempTree()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write_bytes(const std::filesystem::path& file, std::size_t count)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << std::string(count, 'x');
    REQUIRE(stream.good());
}

void set_age(const std::filesystem::path& file, std::chrono::seconds age)
{
    std::error_code error;
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now() - age, error);
    REQUIRE_FALSE(error);
}

}  // namespace

TEST_CASE("CVF-005 save reports image_write_error when the managed root is gone",
          "[cvf-005][artifacts][failure]")
{
    TempTree tree{"write_failure"};
    const auto root = tree.path() / "captures";
    auto created = art::CaptureStore::create(root);
    REQUIRE(created.has_value());
    std::unique_ptr<art::CaptureStore> store = std::move(created.value());

    std::error_code error;
    std::filesystem::remove_all(root, error);
    REQUIRE_FALSE(error);
    REQUIRE_FALSE(std::filesystem::exists(root));

    const cv::Mat frame(2, 2, CV_8UC3, cv::Scalar(1, 2, 3));
    auto saved = store->save(art::CaptureSaveRequest{.pixels = frame,
                                                     .recipe_id = "recipe.alpha",
                                                     .request_id = "request.beta",
                                                     .sequence = 0u});

    REQUIRE_FALSE(saved.has_value());
    CHECK(saved.failure().status == core::Status::internal_error);
    CHECK(saved.failure().code == core::ErrorCode::image_write_error);
}

TEST_CASE("CVF-005 retention is non-recursive and only handles root regular files",
          "[cvf-005][retention]")
{
    TempTree tree{"non_recursive"};
    const auto root = tree.path() / "captures";
    const auto nested = root / "nested";
    REQUIRE(std::filesystem::create_directories(nested));
    const auto top = root / "20240101T000000_recipe_request_1.png";
    const auto deep = nested / "20240101T000000_recipe_request_2.png";
    write_bytes(top, 100u);
    write_bytes(deep, 100u);
    set_age(top, 5h);
    set_age(deep, 5h);

    auto deleted = art::CaptureStore::enforce_retention(root, 60s, std::uint64_t{1} << 40);

    REQUIRE(deleted.has_value());
    CHECK(deleted.value() == 1u);
    CHECK_FALSE(std::filesystem::exists(top));
    CHECK(std::filesystem::is_regular_file(deep));
}

TEST_CASE("CVF-005 degraded diagnostics keeps the log path contract and never throws",
          "[cvf-005][diagnostics][failure]")
{
    TempTree tree{"degraded_path"};
    const auto blocker = tree.path() / "blocker";
    write_bytes(blocker, 8u);
    const auto log_dir = blocker / "logs";

    auto created = diag::Diagnostics::create(
        diag::DiagnosticsConfig{.level = core::LogLevel::info,
                                .max_file_bytes = 1024u,
                                .max_files = 1u,
                                .log_dir = log_dir},
        diag::CallbackBinding{});
    REQUIRE(created.has_value());
    std::unique_ptr<diag::Diagnostics> diagnostics = std::move(created.value());

    CHECK(std::filesystem::path{diagnostics->log_file_path()} == log_dir / "cvforwin.log");
    CHECK((diagnostics->warnings() & static_cast<std::uint32_t>(core::warning_log_sink_failed)) != 0u);
    CHECK_NOTHROW(diagnostics->log(core::LogLevel::error, "cvf005 developer degraded probe"));
}

TEST_CASE("CVF-005 a root with a trailing separator still yields direct children",
          "[cvf-005][artifacts]")
{
    TempTree tree{"trailing_separator"};
    const auto root = tree.path() / "captures" / "";
    const auto expected_root = tree.path() / "captures";
    auto created = art::CaptureStore::create(root);
    REQUIRE(created.has_value());
    std::unique_ptr<art::CaptureStore> store = std::move(created.value());

    const cv::Mat frame(2, 2, CV_8UC3, cv::Scalar(7, 7, 7));
    auto saved = store->save(art::CaptureSaveRequest{.pixels = frame,
                                                     .recipe_id = "recipe",
                                                     .request_id = "request",
                                                     .sequence = 3u});

    REQUIRE(saved.has_value());
    const std::filesystem::path file{saved.value()};
    CHECK(file.is_absolute());
    CHECK(file.parent_path() == expected_root);
    CHECK(file.filename().string().ends_with("_recipe_request_3.png"));
    CHECK(std::filesystem::is_regular_file(file));
}

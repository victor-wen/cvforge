/*
 * CVF-104 independent black-box end-to-end tests through the runtime context,
 * using the deterministic synthetic camera override and an injected
 * ArtifactSink (RuntimeOptions.artifact_sink).
 *
 * Coverage: B1 (PASS/never publishes nothing and is warning-free), B2
 * (FAIL/fail_or_error commits an image inside the managed root), B8
 * (elapsed/return happen after finalization and no retention scan/delete occurs
 * before inspect returns), and B9 (repeated inspections stay bounded; retention
 * is not awaited by inspect and eventually removes an aged file).
 *
 * Only frozen interface headers are included. No production .cpp is read.
 */

#include <catch2/catch_test_macros.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

#include "cvf104_test_support.h"

using namespace cvf104;

namespace {

std::filesystem::path managed_root_of(const rt::InspectionOutcome& outcome)
{
    REQUIRE_FALSE(outcome.image_path.empty());
    return std::filesystem::path(outcome.image_path).parent_path();
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* B1: PASS with save_policy never publishes nothing and is warning-free.     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B1: a PASS with save_policy never returns no image_path and warning-free OK",
          "[cvf-104][B1][e2e]")
{
    TempDir sink_root("b1_sink");
    auto sink = std::make_shared<TestSink>(sink_root.path() / "captures", TestSink::Mode::ambient);
    RuntimeHarness harness("b1_never_pass", "example.pass", pass_parameters(), "never", false, 30,
                           1073741824ull, sink);

    const RequestHolder request("example.pass", "b1-pass", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::pass);
    CHECK(outcome.error_code == core::ErrorCode::none);
    CHECK(outcome.image_path.empty());
    CHECK(outcome.warning_flags == 0u);
    CHECK(sink->write_calls() == 0u);
    CHECK(sink->commit_calls() == 0u);
    CHECK(sink->discard_calls() == 0u);
    CHECK(count_regular_files(sink->root()) == 0u);
}

/* ------------------------------------------------------------------------- */
/* B2: FAIL with fail_or_error commits an image inside the managed captures root. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B2: a FAIL with fail_or_error returns OK with a committed image_path inside the "
          "captures root",
          "[cvf-104][B2][e2e]")
{
    TempDir sink_root("b2_sink");
    auto sink = std::make_shared<TestSink>(sink_root.path() / "captures", TestSink::Mode::ambient);
    RuntimeHarness harness("b2_fail_or_error", "example.fail", fail_parameters(), "fail_or_error",
                           false, 30, 1073741824ull, sink);

    const RequestHolder request("example.fail", "b2-fail", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    REQUIRE(outcome.status == core::Status::ok);
    REQUIRE(outcome.verdict == core::Verdict::fail);
    REQUIRE_FALSE(outcome.image_path.empty());

    const std::filesystem::path file = outcome.image_path;
    CHECK(file.is_absolute());
    CHECK(path_within(sink->root(), file));
    REQUIRE(std::filesystem::is_regular_file(file));
    CHECK(sink->committed_with_recipe("example.fail") == 1u);
    CHECK(sink->outstanding() == 0u);

    const cv::Mat decoded = cv::imread(file.string(), cv::IMREAD_UNCHANGED);
    REQUIRE_FALSE(decoded.empty());
    CHECK(decoded.cols == kFrameWidth);
    CHECK(decoded.rows == kFrameHeight);
    CHECK(decoded.channels() == 3);
}

/* ------------------------------------------------------------------------- */
/* B8: inspect does not return until finalization completes, elapsed covers it, */
/* and no retention scan/delete happens before it returns.                     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B8: inspect waits for finalization, measures it in elapsed_ms, and never runs "
          "retention before returning",
          "[cvf-104][B8][e2e]")
{
    TempDir sink_root("b8_sink");
    auto sink = std::make_shared<TestSink>(sink_root.path() / "captures", TestSink::Mode::ambient);
    RuntimeHarness harness("b8_finalization", "example.fail", fail_parameters(), "fail_or_error",
                           false, 1, 1073741824ull, sink);

    /* Discover the managed captures root through one completed save. */
    const RequestHolder warmup_request("example.fail", "b8-warmup", 5000u);
    const rt::InspectionOutcome warmup = harness.inspect(warmup_request.get());
    REQUIRE(warmup.status == core::Status::ok);
    REQUIRE_FALSE(warmup.image_path.empty());
    const std::filesystem::path managed_root = managed_root_of(warmup);

    /* Seed an aged file in the same managed root that synchronous retention
     * would immediately delete. */
    const auto aged = managed_root / "20240101T000000_seed_req_0.png";
    write_bytes(aged, 128u);
    set_age(aged, std::chrono::hours(24 * 10));
    REQUIRE(std::filesystem::is_regular_file(aged));

    /* Now make the sink block, so the next inspect must wait inside finalization. */
    sink->set_mode(TestSink::Mode::block_write);

    const RequestHolder request("example.fail", "b8-blocking", 5000u);
    std::atomic<bool> returned{false};
    rt::InspectionOutcome outcome{};
    std::thread worker([&] {
        outcome = harness.inspect(request.get());
        returned.store(true, std::memory_order_release);
    });

    REQUIRE(sink->wait_entered(std::chrono::milliseconds(3000)));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_FALSE(returned.load(std::memory_order_acquire));

    /* No retention scan/delete can have run: the aged file is still there. */
    CHECK(std::filesystem::is_regular_file(aged));

    sink->release();
    worker.join();

    CHECK(returned.load(std::memory_order_acquire));
    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::fail);
    CHECK_FALSE(outcome.image_path.empty());
    /* The blocked save was inside the finalization window, so it is measured in
     * elapsed_ms and the call returns only after it completed. */
    CHECK(outcome.elapsed_ms >= 100u);

    /* Retention still must not have run synchronously during the call. */
    CHECK(std::filesystem::is_regular_file(aged));
    CHECK(sink->outstanding() == 0u);
}

/* ------------------------------------------------------------------------- */
/* B9: repeated inspections stay bounded and do not await retention.           */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 B9: repeated inspections commit one bounded file each and never await retention",
          "[cvf-104][B9][e2e]")
{
    TempDir sink_root("b9_repeat");
    auto sink = std::make_shared<TestSink>(sink_root.path() / "captures", TestSink::Mode::ambient);
    RuntimeHarness harness("b9_repeat", "example.fail", fail_parameters(), "fail_or_error", false, 1,
                           1073741824ull, sink);

    const RequestHolder first_request("example.fail", "b9-first", 5000u);
    const rt::InspectionOutcome first = harness.inspect(first_request.get());
    REQUIRE(first.status == core::Status::ok);
    const std::filesystem::path managed_root = managed_root_of(first);

    const auto aged = managed_root / "20240101T000000_seed_req_9.png";
    write_bytes(aged, 128u);
    set_age(aged, std::chrono::hours(24 * 10));

    constexpr int kRounds = 8;
    std::filesystem::path previous = first.image_path;
    for (int round = 1; round < kRounds; ++round) {
        const RequestHolder request("example.fail", "b9-" + std::to_string(round), 5000u);
        const rt::InspectionOutcome outcome = harness.inspect(request.get());

        REQUIRE(outcome.status == core::Status::ok);
        REQUIRE(outcome.verdict == core::Verdict::fail);
        REQUIRE_FALSE(outcome.image_path.empty());
        CHECK(path_within(managed_root, outcome.image_path));
        CHECK(outcome.image_path != previous);
        previous = outcome.image_path;
    }

    /* Commits are bounded (one per call), nothing is left in flight, and no
     * temporary was orphaned. */
    CHECK(sink->committed_with_recipe("example.fail") == static_cast<std::size_t>(kRounds));
    CHECK(sink->outstanding() == 0u);
    CHECK(sink->write_calls() == static_cast<std::uint32_t>(kRounds));
    CHECK(sink->commit_calls() == static_cast<std::uint32_t>(kRounds));
    CHECK(count_files_with_extension(managed_root, ".png") ==
          static_cast<std::size_t>(kRounds) + 1u);
    /* Far below both triggers, so background retention has not run. */
    CHECK(std::filesystem::is_regular_file(aged));
}

TEST_CASE("CVF-104 B9: reaching the capture trigger runs retention off the caller path and removes "
          "the aged file",
          "[cvf-104][B9][e2e][retention]")
{
    TempDir sink_root("b9_trigger");
    auto sink = std::make_shared<TestSink>(sink_root.path() / "captures", TestSink::Mode::ambient);
    RuntimeHarness harness("b9_trigger", "example.fail", fail_parameters(), "fail_or_error", false,
                           1, 1073741824ull, sink);

    const RequestHolder first_request("example.fail", "b9t-first", 5000u);
    const rt::InspectionOutcome first = harness.inspect(first_request.get());
    REQUIRE(first.status == core::Status::ok);
    const std::filesystem::path managed_root = managed_root_of(first);

    const auto aged = managed_root / "20240101T000000_seed_req_100.png";
    write_bytes(aged, 256u);
    set_age(aged, std::chrono::hours(24 * 10));
    REQUIRE(std::filesystem::is_regular_file(aged));

    /* Reach the 100-committed-capture trigger (the first commit is done). */
    for (int round = 1; round < 100; ++round) {
        const RequestHolder request("example.fail", "b9t-" + std::to_string(round), 5000u);
        const rt::InspectionOutcome outcome = harness.inspect(request.get());
        REQUIRE(outcome.status == core::Status::ok);
    }

    /* Retention runs on a background worker and is never awaited by inspect, so
     * the caller returns first and the aged file is removed asynchronously. */
    bool removed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!std::filesystem::exists(aged)) {
            removed = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    CHECK(removed);
    CHECK(sink->committed_with_recipe("example.fail") == 100u);
    CHECK(sink->outstanding() == 0u);
}

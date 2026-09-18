/*
 * CVF-104 independent black-box unit tests for the frozen save-worker seam
 * src/artifacts/save_worker.h.
 *
 * Coverage: the one-executing + one-queued bound (try_submit saturation),
 * deadline-honoring wait_until, completed-save publication inside the managed
 * root, cancel_pending publishing no final file, failed writes, and a
 * deadlock-free drain that leaves no temporary file. The frozen ArtifactSink is
 * replaced by a deterministic test double (block until released / fail / real
 * CaptureStore below the managed root).
 *
 * Only the frozen seam headers are included. No production .cpp is read.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#include "cvf104_test_support.h"

using namespace cvf104;

namespace {

art::SaveJob make_job(const std::string& recipe_id, const std::string& request_id,
                      std::uint64_t sequence)
{
    art::SaveJob job;
    job.pixels = cv::Mat(kFrameHeight, kFrameWidth, CV_8UC3, cv::Scalar(10, 20, 30));
    job.recipe_id = recipe_id;
    job.request_id = request_id;
    job.sequence = sequence;
    return job;
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* One executing + one queued: a third submit is rejected while saturated.    */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 worker: at most one save executes and one is queued; a third submit is rejected",
          "[cvf-104][worker][saturation]")
{
    TempDir root("worker_saturation");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::block_write);
    art::SaveWorker worker(*sink);

    CHECK(worker.executing() == 0u);
    CHECK(worker.queued() == 0u);

    CHECK(worker.try_submit(make_job("recipe.a", "req-1", 1)));
    REQUIRE(sink->wait_entered(std::chrono::milliseconds(2000)));
    CHECK(worker.executing() == 1u);
    CHECK(worker.queued() == 0u);

    /* Second job is accepted into the single queued slot. */
    CHECK(worker.try_submit(make_job("recipe.b", "req-2", 2)));
    CHECK(worker.queued() == 1u);
    CHECK(worker.executing() == 1u);

    /* Third job has nowhere to go: saturation. */
    CHECK_FALSE(worker.try_submit(make_job("recipe.c", "req-3", 3)));
    CHECK(worker.executing() == 1u);
    CHECK(worker.queued() == 1u);

    sink->release();
    worker.drain();
    CHECK(sink->outstanding() == 0u);
}

/* ------------------------------------------------------------------------- */
/* A completed save publishes a path inside the managed root.                 */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 worker: a completed save returns a committed path inside the managed root",
          "[cvf-104][worker][completed]")
{
    TempDir root("worker_completed");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::ambient);
    art::SaveWorker worker(*sink);

    REQUIRE(worker.try_submit(make_job("recipe.done", "req-done", 7)));

    const art::SaveOutcome outcome = worker.wait_until(core::Deadline::from_timeout_ms(5000));

    CHECK(outcome.state == art::SaveState::completed);
    CHECK(outcome.error == core::ErrorCode::none);
    REQUIRE_FALSE(outcome.path.empty());
    CHECK(outcome.path.is_absolute());
    CHECK(path_within(sink->root(), outcome.path));
    CHECK(std::filesystem::is_regular_file(outcome.path));
    CHECK(sink->write_calls() >= 1u);
    CHECK(sink->commit_calls() >= 1u);
    CHECK(sink->committed_with_recipe("recipe.done") == 1u);

    worker.drain();
    CHECK(sink->outstanding() == 0u);
}

/* ------------------------------------------------------------------------- */
/* An encode/write failure reaches a failed terminal state with no path.      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 worker: an injected write failure yields a failed outcome with no published path",
          "[cvf-104][worker][failure]")
{
    TempDir root("worker_failure");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::fail_write);
    art::SaveWorker worker(*sink);

    REQUIRE(worker.try_submit(make_job("recipe.fail", "req-fail", 9)));

    const art::SaveOutcome outcome = worker.wait_until(core::Deadline::from_timeout_ms(5000));

    CHECK(outcome.state == art::SaveState::failed);
    CHECK(outcome.path.empty());
    CHECK(outcome.error != core::ErrorCode::none);
    CHECK(sink->commit_calls() == 0u);

    worker.drain();
    CHECK(sink->outstanding() == 0u);
}

/* ------------------------------------------------------------------------- */
/* wait_until honors the deadline while the sink is blocked.                  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 worker: wait_until returns within the deadline when the save cannot complete",
          "[cvf-104][worker][deadline]")
{
    TempDir root("worker_deadline");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::block_write);
    art::SaveWorker worker(*sink);

    REQUIRE(worker.try_submit(make_job("recipe.slow", "req-slow", 11)));
    REQUIRE(sink->wait_entered(std::chrono::milliseconds(2000)));

    const auto start = std::chrono::steady_clock::now();
    const art::SaveOutcome outcome = worker.wait_until(core::Deadline::from_timeout_ms(150));
    const auto wall = wall_ms_since(start);

    CHECK(wall >= 100);
    CHECK(wall < 2000);
    CHECK(outcome.state != art::SaveState::completed);
    CHECK(outcome.path.empty());

    /* Release the blocked sink so the worker can finish before teardown. */
    sink->release();
    worker.drain();
    CHECK(sink->outstanding() == 0u);
}

/* ------------------------------------------------------------------------- */
/* cancel_pending publishes no final file for the queued job.                 */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 worker: cancel_pending drops the queued job and never publishes its file",
          "[cvf-104][worker][cancel]")
{
    TempDir root("worker_cancel");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::block_write);
    art::SaveWorker worker(*sink);

    REQUIRE(worker.try_submit(make_job("recipe.execute", "req-exec", 21)));
    REQUIRE(sink->wait_entered(std::chrono::milliseconds(2000)));
    REQUIRE(worker.try_submit(make_job("recipe.cancel", "req-cancel", 22)));
    REQUIRE(worker.queued() == 1u);

    worker.cancel_pending();
    CHECK(worker.queued() == 0u);

    sink->release();
    worker.drain();

    /* The executing job published; the cancelled queued job never did. */
    CHECK(sink->committed_with_recipe("recipe.execute") == 1u);
    CHECK(sink->committed_with_recipe("recipe.cancel") == 0u);
    CHECK(sink->outstanding() == 0u);
}

/* ------------------------------------------------------------------------- */
/* drain does not deadlock and leaves no temporary file.                      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 negative: shutdown during an in-flight save does not deadlock and leaves no temp",
          "[cvf-104][worker][negative][shutdown]")
{
    TempDir root("worker_shutdown");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::block_write);
    art::SaveWorker worker(*sink);

    REQUIRE(worker.try_submit(make_job("recipe.inflight", "req-inflight", 31)));
    REQUIRE(sink->wait_entered(std::chrono::milliseconds(2000)));
    REQUIRE(worker.try_submit(make_job("recipe.queued", "req-queued", 32)));

    std::atomic<bool> drained{false};
    std::thread shutdown_thread([&] {
        worker.drain();
        drained.store(true, std::memory_order_release);
    });

    /* Unblock the in-flight write; drain must then complete and join. */
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sink->release();
    shutdown_thread.join();

    CHECK(drained.load(std::memory_order_acquire));
    CHECK(sink->outstanding() == 0u);
    CHECK(sink->write_calls() >= 1u);

    /* Every tracked temporary was either committed or discarded. */
    const std::size_t on_disk = count_regular_files(sink->root());
    CHECK(on_disk == sink->commit_calls());
}

/* ------------------------------------------------------------------------- */
/* Verify-phase additions (2026-09-19, owner: test-engineer).                 */
/* Requirement-derived edge cases from the CVF-104 brief. No existing         */
/* assertion changed.                                                         */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 worker boundary: wait_until with an already-expired deadline returns at once "
          "without completing the blocked save",
          "[cvf-104][worker][deadline][boundary]")
{
    TempDir root("worker_expired");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::block_write);
    art::SaveWorker worker(*sink);

    REQUIRE(worker.try_submit(make_job("recipe.expired", "req-expired", 41)));
    REQUIRE(sink->wait_entered(std::chrono::milliseconds(2000)));

    const auto start = std::chrono::steady_clock::now();
    const art::SaveOutcome outcome = worker.wait_until(core::Deadline::immediate());
    const auto wall = wall_ms_since(start);

    /* An expired deadline must not wait for the blocked write. */
    CHECK(wall < 1000);
    CHECK(outcome.state != art::SaveState::completed);
    CHECK(outcome.path.empty());

    sink->release();
    worker.drain();
    CHECK(sink->outstanding() == 0u);
}

TEST_CASE("CVF-104 worker boundary: cancel_pending with nothing queued is a safe no-op",
          "[cvf-104][worker][cancel][boundary]")
{
    TempDir root("worker_cancel_empty");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::ambient);
    art::SaveWorker worker(*sink);

    CHECK(worker.executing() == 0u);
    CHECK(worker.queued() == 0u);

    worker.cancel_pending();

    CHECK(worker.executing() == 0u);
    CHECK(worker.queued() == 0u);

    /* The worker is not wedged: a later submit still completes. */
    REQUIRE(worker.try_submit(make_job("recipe.after", "req-after", 42)));
    const art::SaveOutcome outcome = worker.wait_until(core::Deadline::from_timeout_ms(5000));
    CHECK(outcome.state == art::SaveState::completed);
    CHECK(sink->committed_with_recipe("recipe.after") == 1u);

    worker.drain();
    CHECK(sink->outstanding() == 0u);
}

TEST_CASE("CVF-104 worker boundary: drain is idempotent when called twice",
          "[cvf-104][worker][drain][boundary]")
{
    TempDir root("worker_drain_twice");
    auto sink = std::make_shared<TestSink>(root.path() / "captures", TestSink::Mode::ambient);
    art::SaveWorker worker(*sink);

    REQUIRE(worker.try_submit(make_job("recipe.drain", "req-drain", 43)));
    const art::SaveOutcome outcome = worker.wait_until(core::Deadline::from_timeout_ms(5000));
    REQUIRE(outcome.state == art::SaveState::completed);

    worker.drain();
    worker.drain();

    CHECK(worker.executing() == 0u);
    CHECK(worker.queued() == 0u);
    CHECK(sink->outstanding() == 0u);
    CHECK(count_regular_files(sink->root()) == sink->commit_calls());
}

/* ------------------------------------------------------------------------- */
/* Verify-phase Windows fix (2026-09-19, owner: test-engineer).               */
/* path_within must accept both '/' and '\\' as separator boundaries: the     */
/* original raw-string comparison only accepted '/', so it failed on native   */
/* Windows, where lexically_normal().string() uses '\\'.                      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 support: path_within is separator-agnostic and still rejects siblings and "
          "escapes",
          "[cvf-104][support][path_within]")
{
    const std::filesystem::path root = "/tmp/cvf104_path_within/captures";

    /* Root itself and native forward-slash descendants. */
    CHECK(path_within(root, root));
    CHECK(path_within(root, root / "frame.png"));
    CHECK(path_within(root, root / "nested" / "frame.png"));

    /* Windows-style backslash separators must be recognized as boundaries. */
    CHECK(path_within(root, std::filesystem::path(root.string() + "\\frame.png")));
    CHECK(path_within(std::filesystem::path("C:\\root\\captures"),
                      std::filesystem::path("C:\\root\\captures\\frame.png")));

    /* A sibling that only extends root's final component name is outside. */
    CHECK_FALSE(
        path_within(root, std::filesystem::path("/tmp/cvf104_path_within/captures_x/frame.png")));
    CHECK_FALSE(path_within(std::filesystem::path("C:\\root\\captures"),
                            std::filesystem::path("C:\\root\\captures_x\\frame.png")));

    /* An escape out of root is outside. */
    CHECK_FALSE(
        path_within(root, std::filesystem::path("/tmp/cvf104_path_within/other/frame.png")));
    CHECK_FALSE(path_within(root, root / ".." / "outside.png"));
}

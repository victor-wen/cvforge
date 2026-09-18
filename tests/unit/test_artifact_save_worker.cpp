/*
 * Developer-owned regression tests for the CVF-104 production artifact sink and
 * its bounded save worker.
 *
 * The independent CVF-104 suite injects its own ArtifactSink test double, so the
 * production CaptureStoreSink (encode -> same-root temp -> atomic rename ->
 * discard on cancellation/failure) and the SaveWorker integration are covered
 * here. The cases assert the observable file-system result: a committed PNG that
 * decodes, sanitized in-root names, the temporary always removed, and a failed
 * commit reported as image_write_error.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include "artifacts/capture_sink.h"
#include "artifacts/capture_store.h"
#include "artifacts/save_worker.h"
#include "core/deadline.h"
#include "core/error.h"
#include "core/status.h"

namespace {

namespace art = cvforwin::artifacts;
namespace core = cvforwin::core;

constexpr int kWidth = 16;
constexpr int kHeight = 12;

class TempTree {
public:
    explicit TempTree(std::string_view tag)
    {
        static std::atomic<std::uint64_t> counter{0u};
        path_ = std::filesystem::temp_directory_path() /
                ("cvf104_dev_" + std::string(tag) + "_" + std::to_string(counter.fetch_add(1u)));
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        REQUIRE_FALSE(error);
        REQUIRE(std::filesystem::create_directories(path_, error));
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

art::SaveJob make_job(std::string recipe_id, std::string request_id, std::uint64_t sequence)
{
    art::SaveJob job;
    job.pixels = cv::Mat(kHeight, kWidth, CV_8UC3, cv::Scalar(4, 8, 12));
    job.recipe_id = std::move(recipe_id);
    job.request_id = std::move(request_id);
    job.sequence = sequence;
    return job;
}

std::size_t count_temporary_files(const std::filesystem::path& root)
{
    std::size_t count = 0;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(root, error); iterator !=
                                                                    std::filesystem::directory_iterator();
         iterator.increment(error)) {
        const std::filesystem::path entry = iterator->path();
        if (art::CaptureStore::is_temporary_capture_name(entry.filename().string())) {
            ++count;
        }
    }
    return count;
}

std::unique_ptr<art::CaptureStore> make_store(const std::filesystem::path& root)
{
    auto created = art::CaptureStore::create(root);
    REQUIRE(created.has_value());
    return std::move(created.value());
}

/*
 * Sink whose write_temp blocks until release() is called, then returns a real
 * temporary file. commit() renames that temporary to a distinct final path so a
 * caller can observe on disk whether the late job ever published. Used to pin
 * the B-1 property: an expired wait_until must cancel the in-flight job.
 */
class BlockingWriteSink final : public art::ArtifactSink {
public:
    explicit BlockingWriteSink(std::filesystem::path root)
        : root_(std::move(root))
    {
        std::error_code error;
        std::filesystem::create_directories(root_, error);
    }

    core::Result<std::filesystem::path> write_temp(const art::SaveJob& job) override
    {
        (void)job;
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            entered_ = true;
        }
        gate_cv_.notify_all();
        {
            std::unique_lock<std::mutex> lock(gate_mutex_);
            gate_cv_.wait(lock, [this] { return released_; });
        }

        const std::filesystem::path temporary = temporary_path();
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << "partial";
        stream.close();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++write_calls_;
        }
        return temporary;
    }

    core::Result<std::filesystem::path> commit(const std::filesystem::path& temp) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++commit_calls_;
        const std::filesystem::path final = root_ / "published.png";
        std::error_code error;
        std::filesystem::rename(temp, final, error);
        return final;
    }

    void discard(const std::filesystem::path& temp) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++discard_calls_;
        std::error_code error;
        std::filesystem::remove(temp, error);
    }

    bool wait_entered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(gate_mutex_);
        return gate_cv_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            released_ = true;
        }
        gate_cv_.notify_all();
    }

    std::filesystem::path temporary_path() const
    {
        return root_ / ".cvftmp_dev_block.part";
    }

    std::filesystem::path published_path() const
    {
        return root_ / "published.png";
    }

    std::uint32_t write_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return write_calls_;
    }

    std::uint32_t commit_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return commit_calls_;
    }

    std::uint32_t discard_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return discard_calls_;
    }

private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::uint32_t write_calls_ = 0;
    std::uint32_t commit_calls_ = 0;
    std::uint32_t discard_calls_ = 0;

    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    bool entered_ = false;
    bool released_ = false;
};

/*
 * Sink whose commit() blocks until release_commit() is called, then publishes a
 * real final file by rename. It pins the cancellation/commit race: a wait_until
 * deadline that expires while the commit is in flight must either win (no final
 * file) or be reported as the completed outcome that actually published, never
 * as a synthetic cancellation next to a published file.
 */
class BlockingCommitSink final : public art::ArtifactSink {
public:
    explicit BlockingCommitSink(std::filesystem::path root)
        : root_(std::move(root))
    {
        std::error_code error;
        std::filesystem::create_directories(root_, error);
    }

    core::Result<std::filesystem::path> write_temp(const art::SaveJob& job) override
    {
        (void)job;
        const std::filesystem::path temporary = temporary_path();
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << "partial";
        stream.close();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++write_calls_;
        }
        return temporary;
    }

    core::Result<std::filesystem::path> commit(const std::filesystem::path& temp) override
    {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            commit_entered_ = true;
        }
        gate_cv_.notify_all();
        {
            std::unique_lock<std::mutex> lock(gate_mutex_);
            gate_cv_.wait(lock, [this] { return commit_released_; });
        }

        const std::filesystem::path final = published_path();
        std::error_code error;
        std::filesystem::rename(temp, final, error);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++commit_calls_;
        }
        return final;
    }

    void discard(const std::filesystem::path& temp) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++discard_calls_;
        std::error_code error;
        std::filesystem::remove(temp, error);
    }

    bool wait_commit_entered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(gate_mutex_);
        return gate_cv_.wait_for(lock, timeout, [this] { return commit_entered_; });
    }

    void release_commit()
    {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            commit_released_ = true;
        }
        gate_cv_.notify_all();
    }

    std::filesystem::path temporary_path() const
    {
        return root_ / ".cvftmp_dev_commit.part";
    }

    std::filesystem::path published_path() const
    {
        return root_ / "published.png";
    }

    std::uint32_t write_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return write_calls_;
    }

    std::uint32_t commit_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return commit_calls_;
    }

    std::uint32_t discard_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return discard_calls_;
    }

private:
    std::filesystem::path root_;
    mutable std::mutex mutex_;
    std::uint32_t write_calls_ = 0;
    std::uint32_t commit_calls_ = 0;
    std::uint32_t discard_calls_ = 0;

    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    bool commit_entered_ = false;
    bool commit_released_ = false;
};

}  // namespace

TEST_CASE("CVF-104 developer: a completed sink save publishes a decodable PNG and removes its temp",
          "[cvf-104][dev][worker][sink]")
{
    TempTree tree("sink_completed");
    const auto root = tree.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = make_store(root);
    art::CaptureStoreSink sink(*store);
    art::SaveWorker worker(sink);

    REQUIRE(worker.try_submit(make_job("example.fail", "dev-complete", 5u)));
    const art::SaveOutcome outcome = worker.wait_until(core::Deadline::from_timeout_ms(5000u));

    REQUIRE(outcome.state == art::SaveState::completed);
    CHECK(outcome.error == core::ErrorCode::none);
    REQUIRE_FALSE(outcome.path.empty());
    CHECK(outcome.path.is_absolute());
    CHECK(outcome.path.parent_path() == root.lexically_normal());
    REQUIRE(std::filesystem::is_regular_file(outcome.path));

    const cv::Mat decoded = cv::imread(outcome.path.string(), cv::IMREAD_UNCHANGED);
    REQUIRE_FALSE(decoded.empty());
    CHECK(decoded.cols == kWidth);
    CHECK(decoded.rows == kHeight);
    CHECK(decoded.channels() == 3);

    /* The published temp was renamed away; no unpublished temporary remains. */
    CHECK(count_temporary_files(root) == 0u);

    worker.drain();
}

TEST_CASE("CVF-104 developer: sink identifiers are sanitized and stay a direct child of the root",
          "[cvf-104][dev][sink][negative]")
{
    TempTree tree("sink_sanitize");
    const auto root = tree.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = make_store(root);
    art::CaptureStoreSink sink(*store);

    auto temporary = sink.write_temp(make_job("../evil", "a/b", 1u));
    REQUIRE(temporary.has_value());
    CHECK(temporary.value().parent_path() == root.lexically_normal());
    CHECK(art::CaptureStore::is_temporary_capture_name(temporary.value().filename().string()));

    auto committed = sink.commit(temporary.value());
    REQUIRE(committed.has_value());
    const std::filesystem::path file = committed.value();
    CHECK(file.parent_path() == root.lexically_normal());
    CHECK(file.filename().string().ends_with("_1.png"));

    const std::string name = file.filename().string();
    CHECK(name.find('/') == std::string::npos);
    CHECK(name.find('\\') == std::string::npos);
    /* A single filename component is never a traversal token; embedded dots are
     * legal sanitized bytes (see CaptureStore's identifier rules). */
    CHECK(name != ".");
    CHECK(name != "..");
    CHECK(file.filename() == std::filesystem::path(name));
    CHECK(std::filesystem::is_regular_file(file));
    CHECK_FALSE(std::filesystem::exists(temporary.value()));
    CHECK(count_temporary_files(root) == 0u);
}

TEST_CASE("CVF-104 developer: discard removes an unpublished temporary and leaves the root clean",
          "[cvf-104][dev][sink][cancel]")
{
    TempTree tree("sink_discard");
    const auto root = tree.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = make_store(root);
    art::CaptureStoreSink sink(*store);

    auto temporary = sink.write_temp(make_job("example.pass", "dev-discard", 2u));
    REQUIRE(temporary.has_value());
    REQUIRE(std::filesystem::is_regular_file(temporary.value()));

    sink.discard(temporary.value());

    CHECK_FALSE(std::filesystem::exists(temporary.value()));
    CHECK(count_temporary_files(root) == 0u);
}

TEST_CASE("CVF-104 developer: a commit of an unregistered temporary fails with image_write_error",
          "[cvf-104][dev][sink][failure]")
{
    TempTree tree("sink_unregistered");
    const auto root = tree.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = make_store(root);
    art::CaptureStoreSink sink(*store);

    const auto bogus = root / ".cvftmp_unregistered.part";
    auto committed = sink.commit(bogus);

    REQUIRE_FALSE(committed.has_value());
    CHECK(committed.failure().status == core::Status::internal_error);
    CHECK(committed.failure().code == core::ErrorCode::image_write_error);
    CHECK_FALSE(std::filesystem::exists(bogus));
}

TEST_CASE("CVF-104 developer: the worker never publishes after cancellation while the sink is idle",
          "[cvf-104][dev][worker][cancel]")
{
    TempTree tree("worker_idle_cancel");
    const auto root = tree.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = make_store(root);
    art::CaptureStoreSink sink(*store);
    art::SaveWorker worker(sink);

    /* No job submitted: cancel_pending and an empty wait are both inert. */
    worker.cancel_pending();
    const art::SaveOutcome outcome = worker.wait_until(core::Deadline::from_timeout_ms(50u));
    CHECK(outcome.state == art::SaveState::idle);
    CHECK(outcome.path.empty());
    CHECK(worker.executing() == 0u);
    CHECK(worker.queued() == 0u);
    CHECK(count_temporary_files(root) == 0u);

    worker.drain();
}

TEST_CASE("CVF-104 developer regression B-1: an expired wait cancels the in-flight save and never "
          "publishes a late final file",
          "[cvf-104][dev][worker][cancel][regression]")
{
    TempTree tree("worker_cancel_inflight");
    const auto root = tree.path() / "captures";
    BlockingWriteSink sink(root);
    art::SaveWorker worker(sink);

    REQUIRE(worker.try_submit(make_job("example.fail", "dev-cancel-inflight", 71u)));
    /* write_temp has entered and is blocked; the job is still in flight. */
    REQUIRE(sink.wait_entered(std::chrono::milliseconds(2000)));

    const art::SaveOutcome timed_out = worker.wait_until(core::Deadline::from_timeout_ms(100u));
    REQUIRE(timed_out.state == art::SaveState::cancelled);
    CHECK(timed_out.path.empty());

    /* The caller gave up; release the late job so it can reach its terminal state. */
    sink.release();
    worker.drain();

    /* The cancellation propagated: the late job discarded its temp and never committed. */
    CHECK(sink.commit_calls() == 0u);
    CHECK(sink.discard_calls() >= 1u);
    CHECK(sink.write_calls() >= 1u);
    CHECK_FALSE(std::filesystem::exists(sink.published_path()));
    CHECK_FALSE(std::filesystem::exists(sink.temporary_path()));
    CHECK(count_temporary_files(root) == 0u);
    CHECK(worker.executing() == 0u);
    CHECK(worker.queued() == 0u);
}

TEST_CASE("CVF-104 developer regression TOCTOU: a deadline that expires during an in-flight commit "
          "never reports cancelled for the file that was published",
          "[cvf-104][dev][worker][cancel][regression][toctou]")
{
    TempTree tree("worker_commit_race");
    const auto root = tree.path() / "captures";
    BlockingCommitSink sink(root);
    art::SaveWorker worker(sink);

    REQUIRE(worker.try_submit(make_job("example.fail", "dev-commit-race", 91u)));
    /* write_temp produced a temporary and commit() is now in flight and blocked. */
    REQUIRE(sink.wait_commit_entered(std::chrono::milliseconds(2000)));

    /* The caller's deadline expires while the commit is blocked. wait_until must
     * not report a cancellation next to a final file the commit goes on to
     * publish; it either stops the publish or reports the completion. */
    std::optional<art::SaveOutcome> outcome;
    std::thread waiter([&] { outcome = worker.wait_until(core::Deadline::from_timeout_ms(100u)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    sink.release_commit();
    waiter.join();
    worker.drain();

    REQUIRE(outcome.has_value());
    /* The commit decision won the race: the caller is told completed and the
     * reported path is exactly the file that was published. Pre-fix this
     * returned cancelled while published.png still appeared. */
    CHECK(outcome->state == art::SaveState::completed);
    CHECK(outcome->path == sink.published_path());
    CHECK(std::filesystem::is_regular_file(sink.published_path()));
    CHECK(sink.commit_calls() == 1u);
    CHECK(sink.discard_calls() == 0u);
    CHECK_FALSE(std::filesystem::exists(sink.temporary_path()));
    CHECK(count_temporary_files(root) == 0u);
}

/*
 * Bounded background artifact save worker.
 *
 * ArtifactSink is the injected encoder/writer boundary: the production sink
 * encodes PNG, writes a temporary file below the managed root, and publishes it
 * by a same-root atomic rename; tests inject a sink that blocks, fails, or
 * records calls so completion timing is deterministic without touching the
 * real disk.
 *
 * SaveWorker runs at most one save while holding at most one more in a queued
 * slot. The encode/write/commit never executes on the caller thread; a save
 * that is cancelled or fails never publishes a final file, and every temporary
 * file the worker creates is committed or removed.
 */

#ifndef CVFORWIN_SRC_ARTIFACTS_SAVE_WORKER_H_
#define CVFORWIN_SRC_ARTIFACTS_SAVE_WORKER_H_

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include <opencv2/core.hpp>

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"

namespace cvforwin::artifacts {

/* One queued save request; owns the pixels so it can cross a thread boundary. */
struct SaveJob {
    cv::Mat pixels;
    std::string recipe_id;
    std::string request_id;
    std::uint64_t sequence = 0;
};

enum class SaveState { idle,
                       completed,
                       cancelled,
                       failed };

struct SaveOutcome {
    SaveState state = SaveState::idle;
    std::filesystem::path path; /* valid only when completed */
    core::ErrorCode error = core::ErrorCode::none;
};

/*
 * Injected encoder/writer boundary. The production sink encodes PNG, writes a
 * temporary file below the managed root, and publishes it by same-root atomic
 * rename. Tests inject a sink that blocks until released, fails, or records
 * calls, so completion timing is deterministic without touching the real disk.
 */
class ArtifactSink {
public:
    virtual ~ArtifactSink() = default;
    virtual core::Result<std::filesystem::path> write_temp(const SaveJob& job) = 0;
    /* Publishes temp -> final only when not cancelled; returns the final path. */
    virtual core::Result<std::filesystem::path> commit(const std::filesystem::path& temp) = 0;
    /* Removes a temporary file that will not be published. */
    virtual void discard(const std::filesystem::path& temp) noexcept = 0;
};

/*
 * One save worker. At most one save executes and at most one is queued:
 * try_submit returns false when both slots are full. The worker never encodes
 * or writes on the caller thread, never publishes a final file after
 * cancellation, and always removes its temporary file.
 */
class SaveWorker {
public:
    explicit SaveWorker(ArtifactSink& sink);
    ~SaveWorker();

    SaveWorker(const SaveWorker&) = delete;
    SaveWorker& operator=(const SaveWorker&) = delete;

    /* Enqueues one job; false when one save is executing and one is already queued. */
    bool try_submit(SaveJob job);
    /* Blocks until the current job reaches a terminal state or the deadline expires. */
    SaveOutcome wait_until(const core::Deadline& deadline);
    /* Cancels the queued job (if any); no final file is published for it. */
    void cancel_pending() noexcept;
    /* Shutdown: completes the executing job or cancels it, then drains; never deadlocks. */
    void drain() noexcept;

    std::size_t executing() const noexcept;
    std::size_t queued() const noexcept;

private:
    void worker_loop();
    /*
     * Runs one job on the worker thread: writes a temporary, then under
     * publish_mutex_ decides whether to publish or discard it, and stores the
     * terminal outcome for worker_loop to move into completed_.
     */
    void execute(const SaveJob& job, std::uint64_t id) noexcept;
    /* Records a cancellation request for an executing/queued target (mutex_ held). */
    void cancel_target(std::uint64_t target) noexcept;
    /* Both require publish_mutex_ held by the caller. */
    bool is_cancelled_locked(std::uint64_t id) const noexcept;
    bool deadline_expired_locked(std::uint64_t id) const noexcept;

    ArtifactSink& sink_;

    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable state_cv_;

    std::optional<SaveJob> executing_job_;
    std::optional<SaveJob> queued_job_;
    std::uint64_t executing_id_ = 0;
    std::uint64_t queued_id_ = 0;
    std::uint64_t next_id_ = 0;
    std::uint64_t last_submitted_id_ = 0;
    std::map<std::uint64_t, SaveOutcome> completed_;
    bool stopping_ = false;
    std::thread worker_;

    /*
     * Publication gate. Guards the cancellation/deadline decision together with
     * the commit that follows it, so a wait_until deadline cannot mark a job
     * cancelled in the window between execute()'s check and sink_.commit(). It
     * is separate from mutex_ because sink calls may block on I/O: holding it
     * never stalls try_submit/executing/queued, which take only mutex_. Lock
     * order is mutex_ (when held) before publish_mutex_; execute() takes only
     * publish_mutex_.
     */
    std::mutex publish_mutex_;
    /* Job ids whose publication was cancelled before the commit decision. */
    std::set<std::uint64_t> cancelled_ids_;
    /* The job id wait_until is awaiting, and the deadline it waits under. */
    std::uint64_t awaited_id_ = 0;
    std::optional<core::Deadline> awaited_deadline_;
    /*
     * The id whose commit decision completed and its outcome, held until
     * worker_loop moves it into completed_. It lets a wait_until whose deadline
     * expired recover a completed outcome that won the publication race.
     */
    std::uint64_t decided_id_ = 0;
    std::optional<SaveOutcome> decided_outcome_;
};

}  // namespace cvforwin::artifacts

#endif /* CVFORWIN_SRC_ARTIFACTS_SAVE_WORKER_H_ */

#include "artifacts/save_worker.h"

#include <chrono>
#include <cstddef>
#include <utility>

namespace cvforwin::artifacts {

SaveWorker::SaveWorker(ArtifactSink& sink)
    : sink_(sink)
{
    worker_ = std::thread([this] { worker_loop(); });
}

SaveWorker::~SaveWorker()
{
    drain();
}

bool SaveWorker::try_submit(SaveJob job)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
        return false;
    }
    if (!executing_job_.has_value()) {
        const std::uint64_t id = ++next_id_;
        executing_job_ = std::move(job);
        executing_id_ = id;
        last_submitted_id_ = id;
        work_cv_.notify_one();
        return true;
    }
    if (!queued_job_.has_value()) {
        const std::uint64_t id = ++next_id_;
        queued_job_ = std::move(job);
        queued_id_ = id;
        last_submitted_id_ = id;
        return true;
    }
    return false;
}

SaveOutcome SaveWorker::wait_until(const core::Deadline& deadline)
{
    std::unique_lock<std::mutex> lock(mutex_);
    const std::uint64_t target = last_submitted_id_;
    if (target == 0) {
        return SaveOutcome{};
    }

    const auto take = [this, target]() {
        auto found = completed_.find(target);
        if (found == completed_.end()) {
            return SaveOutcome{};
        }
        SaveOutcome outcome = std::move(found->second);
        completed_.erase(found);
        /* ids are issued monotonically and only the newest is ever awaited, so
         * outcomes older than this target can no longer be reclaimed. */
        completed_.erase(completed_.begin(), completed_.lower_bound(target));
        return outcome;
    };

    if (completed_.find(target) != completed_.end()) {
        return take();
    }
    if (!executing_job_.has_value() && !queued_job_.has_value()) {
        return SaveOutcome{};
    }

    /* Publish this job's deadline to the worker before waiting, so execute()
     * can discard the temporary of a save that starts after it expired. */
    {
        std::lock_guard<std::mutex> publish_lock(publish_mutex_);
        awaited_id_ = target;
        awaited_deadline_ = deadline;
    }

    const bool finished =
        state_cv_.wait_for(lock, deadline.remaining(), [this, target] {
            return completed_.find(target) != completed_.end();
        });
    if (!finished) {
        /*
         * The caller has given up. Propagate the cancellation to the target job
         * so it discards its temporary instead of publishing a late final file.
         */
        cancel_target(target);
        /* cancel_target() and the commit decision are mutually exclusive. If the
         * decision already won, the job published; report that instead of a
         * synthetic cancellation so the caller is never told "cancelled" for a
         * job that published. */
        if (completed_.find(target) != completed_.end()) {
            return take();
        }
        {
            std::lock_guard<std::mutex> publish_lock(publish_mutex_);
            if (decided_id_ == target && decided_outcome_.has_value()) {
                SaveOutcome decided = std::move(*decided_outcome_);
                decided_outcome_.reset();
                decided_id_ = 0;
                return decided;
            }
        }
        SaveOutcome timed_out;
        timed_out.state = SaveState::cancelled;
        return timed_out;
    }
    return take();
}

void SaveWorker::cancel_target(std::uint64_t target) noexcept
{
    if (target == 0) {
        return;
    }
    /* mutex_ is held by the caller. */
    if (queued_id_ == target) {
        /* Never executed: just drop it, so it cannot publish. */
        queued_job_.reset();
        queued_id_ = 0;
        return;
    }
    if (executing_id_ != target) {
        return;
    }
    std::lock_guard<std::mutex> publish_lock(publish_mutex_);
    if (decided_id_ == target) {
        /* execute() already made and acted on its commit decision. */
        return;
    }
    cancelled_ids_.insert(target);
}

bool SaveWorker::is_cancelled_locked(std::uint64_t id) const noexcept
{
    return cancelled_ids_.find(id) != cancelled_ids_.end();
}

bool SaveWorker::deadline_expired_locked(std::uint64_t id) const noexcept
{
    return awaited_id_ == id && awaited_deadline_.has_value() && awaited_deadline_->expired();
}

void SaveWorker::cancel_pending() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    queued_job_.reset();
    queued_id_ = 0;
}

void SaveWorker::drain() noexcept
{
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stopping_ = true;
        queued_job_.reset();
        queued_id_ = 0;
        work_cv_.notify_all();
        state_cv_.wait(lock, [this] { return !executing_job_.has_value(); });
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::size_t SaveWorker::executing() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return executing_job_.has_value() ? 1u : 0u;
}

std::size_t SaveWorker::queued() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queued_job_.has_value() ? 1u : 0u;
}

void SaveWorker::execute(const SaveJob& job, std::uint64_t id) noexcept
{
    SaveOutcome outcome;
    std::optional<std::filesystem::path> temporary;

    /* Phase 1: encode/write the temporary outside the publication gate so a
     * wait_until timeout can still mark this job cancelled while it blocks. */
    try {
        core::Result<std::filesystem::path> written = sink_.write_temp(job);
        if (!written.has_value()) {
            outcome.state = SaveState::failed;
            outcome.error = written.failure().code;
        } else {
            temporary = std::move(written).value();
        }
    } catch (...) {
        outcome.state = SaveState::failed;
        outcome.error = core::ErrorCode::internal_exception;
    }

    /* Phase 2: the cancellation/deadline check and the commit are one critical
     * section, so wait_until's cancel_target() is either observed here (discard,
     * no final file) or loses to an already made decision. */
    std::lock_guard<std::mutex> publish_lock(publish_mutex_);
    if (temporary.has_value()) {
        try {
            if (is_cancelled_locked(id) || deadline_expired_locked(id)) {
                /* The caller already gave up: remove the temp and publish nothing. */
                sink_.discard(*temporary);
                outcome.state = SaveState::cancelled;
                outcome.error = core::ErrorCode::none;
            } else {
                core::Result<std::filesystem::path> committed = sink_.commit(*temporary);
                if (!committed.has_value()) {
                    sink_.discard(*temporary);
                    outcome.state = SaveState::failed;
                    outcome.error = committed.failure().code;
                } else {
                    outcome.state = SaveState::completed;
                    outcome.path = std::move(committed).value();
                    outcome.error = core::ErrorCode::none;
                }
            }
        } catch (...) {
            sink_.discard(*temporary);
            outcome.state = SaveState::failed;
            outcome.error = core::ErrorCode::internal_exception;
        }
    }

    decided_id_ = id;
    decided_outcome_ = std::move(outcome);
    cancelled_ids_.erase(id);
    if (awaited_id_ == id) {
        awaited_id_ = 0;
        awaited_deadline_.reset();
    }
}

void SaveWorker::worker_loop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        work_cv_.wait(lock, [this] { return stopping_ || executing_job_.has_value(); });
        if (!executing_job_.has_value()) {
            if (stopping_) {
                return;
            }
            continue;
        }

        const SaveJob job = *executing_job_;
        const std::uint64_t id = executing_id_;
        lock.unlock();
        execute(job, id);
        lock.lock();

        executing_job_.reset();
        executing_id_ = 0;
        {
            std::lock_guard<std::mutex> publish_lock(publish_mutex_);
            if (decided_id_ == id && decided_outcome_.has_value()) {
                completed_[id] = std::move(*decided_outcome_);
                decided_outcome_.reset();
            }
            decided_id_ = 0;
            cancelled_ids_.erase(id);
            if (awaited_id_ == id) {
                awaited_id_ = 0;
                awaited_deadline_.reset();
            }
        }

        if (!stopping_ && queued_job_.has_value()) {
            executing_job_ = std::move(queued_job_);
            executing_id_ = queued_id_;
            queued_job_.reset();
            queued_id_ = 0;
        }
        state_cv_.notify_all();
    }
}

}  // namespace cvforwin::artifacts

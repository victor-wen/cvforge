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
    if (executing_id_ == target) {
        cancelled_ids_.insert(target);
    }
    if (queued_id_ == target) {
        /* Never executed: just drop it, so it cannot publish. */
        queued_job_.reset();
        queued_id_ = 0;
    }
}

bool SaveWorker::is_cancelled(std::uint64_t id) const noexcept
{
    return cancelled_ids_.find(id) != cancelled_ids_.end();
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

SaveOutcome SaveWorker::execute(const SaveJob& job, std::uint64_t id) noexcept
{
    SaveOutcome outcome;
    try {
        core::Result<std::filesystem::path> temporary = sink_.write_temp(job);
        if (!temporary.has_value()) {
            outcome.state = SaveState::failed;
            outcome.error = temporary.failure().code;
            return outcome;
        }

        bool cancelled = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled = is_cancelled(id);
        }
        if (cancelled) {
            /* The caller already gave up: remove the temp and publish nothing. */
            sink_.discard(temporary.value());
            outcome.state = SaveState::cancelled;
            outcome.error = core::ErrorCode::none;
            return outcome;
        }

        core::Result<std::filesystem::path> committed = sink_.commit(temporary.value());
        if (!committed.has_value()) {
            sink_.discard(temporary.value());
            outcome.state = SaveState::failed;
            outcome.error = committed.failure().code;
            return outcome;
        }

        outcome.state = SaveState::completed;
        outcome.path = std::move(committed).value();
        outcome.error = core::ErrorCode::none;
        return outcome;
    } catch (...) {
        outcome.state = SaveState::failed;
        outcome.error = core::ErrorCode::internal_exception;
        return outcome;
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
        SaveOutcome outcome = execute(job, id);
        lock.lock();

        executing_job_.reset();
        executing_id_ = 0;
        cancelled_ids_.erase(id);
        completed_[id] = std::move(outcome);

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

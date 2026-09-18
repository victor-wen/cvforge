/*
 * Portable single-thread command marshaller (uvc_windows_backend module).
 *
 * The UVC backend owns exactly one long-lived worker thread that initializes
 * COM as an MTA and balances one Media Foundation startup/shutdown lifetime.
 * Every backend operation is marshalled onto that thread and completed
 * synchronously for the caller. This header contains only the pure threading
 * machinery, so it has no Windows API, Media Foundation, or OpenCV dependency
 * and is exercised directly by a portable developer unit test.
 *
 * Design:
 *   - start() spawns the thread and runs a caller-supplied startup hook on it;
 *     the caller waits until startup resolves. When startup fails the thread is
 *     joined before start() returns, so no worker is leaked.
 *   - submit() appends one command and waits (bounded by the caller's absolute
 *     deadline) for that command to complete. The command owns its state
 *     through a std::function, so a command that outlives a timed-out caller
 *     never references caller stack storage.
 *   - stop_and_join() signals the loop to finish the running command, discard
 *     queued commands, run the teardown hook on the worker, and join. It is
 *     idempotent and safe to call from any thread.
 */

#ifndef CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_COMMAND_WORKER_H_
#define CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_COMMAND_WORKER_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

#include "core/deadline.h"

namespace cvforwin::camera::uvc {

class CommandWorker {
public:
    /* Outcome of one marshalled command relative to the caller's deadline. */
    enum class SubmitState {
        completed, /* the command ran to completion before the deadline */
        timeout, /* the deadline expired before the command completed */
        unavailable, /* no running worker (stopped, failed, or never started) */
    };

    CommandWorker() = default;

    ~CommandWorker()
    {
        stop_and_join();
    }

    CommandWorker(const CommandWorker&) = delete;
    CommandWorker& operator=(const CommandWorker&) = delete;

    /*
     * Spawns the worker and waits for the startup hook to resolve, bounded by
     * the caller's deadline. Startup runs on the worker; teardown runs on the
     * worker exactly once after the command loop ends, including after a failed
     * startup, so paired startup/teardown side effects (COM/MF lifetime) always
     * balance. Returns true when the worker is running, false otherwise; on a
     * false result the thread has been joined and no worker remains.
     */
    bool start(const core::Deadline& deadline, std::function<bool()> startup, std::function<void()> teardown)
    {
        std::lock_guard<std::mutex> lifecycle(lifecycle_);
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ == State::running) {
            return true;
        }
        if (state_ == State::starting) {
            condition_.wait_for(lock, deadline.remaining(),
                                [this] { return state_ == State::running || state_ == State::failed; });
            return state_ == State::running;
        }
        startup_hook_ = std::move(startup);
        teardown_hook_ = std::move(teardown);
        queue_.clear();
        stop_requested_ = false;
        submitted_sequence_ = 0;
        completed_sequence_ = 0;
        commands_executed_.store(0, std::memory_order_relaxed);
        state_ = State::starting;
        thread_ = std::thread([this] { worker_main(); });
        condition_.wait_for(lock, deadline.remaining(),
                            [this] { return state_ == State::running || state_ == State::failed; });
        const bool started = state_ == State::running;
        if (!started && state_ == State::starting) {
            /* The deadline expired while startup was still resolving. Ask the
             * worker to finish and tear down as soon as startup returns so the
             * join below cannot outlive a resolved startup. */
            stop_requested_ = true;
            condition_.notify_all();
        }
        lock.unlock();
        if (!started && thread_.joinable()) {
            thread_.join();
            lock.lock();
            thread_ = std::thread();
            lock.unlock();
        }
        return started;
    }

    /*
     * Executes one command on the worker. Returns completed when the command
     * finished before the deadline, timeout when the deadline expired first
     * (the command may still be queued or running), or unavailable when no
     * worker is running.
     */
    SubmitState submit(const core::Deadline& deadline, std::function<void()> command)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (state_ != State::running) {
            return SubmitState::unavailable;
        }
        if (deadline.expired()) {
            return SubmitState::timeout;
        }
        const std::uint64_t sequence = ++submitted_sequence_;
        queue_.push_back(Command{sequence, std::move(command)});
        condition_.notify_all();
        condition_.wait_for(lock, deadline.remaining(), [this, sequence] {
            return completed_sequence_ >= sequence || state_ != State::running;
        });
        if (completed_sequence_ >= sequence) {
            return SubmitState::completed;
        }
        if (state_ != State::running) {
            return SubmitState::unavailable;
        }
        return SubmitState::timeout;
    }

    /* True while the owned thread is running and usable. */
    bool running() const noexcept
    {
        return running_.load(std::memory_order_acquire);
    }

    /* Thread id of the worker, or a default id when no worker is running. */
    std::thread::id worker_id() const noexcept
    {
        return worker_id_.load(std::memory_order_relaxed);
    }

    /* True when the calling thread is the owned worker thread. */
    bool on_worker_thread() const noexcept
    {
        return std::this_thread::get_id() == worker_id();
    }

    /* Number of commands executed on the worker since the last start. */
    std::uint64_t commands_executed() const noexcept
    {
        return commands_executed_.load(std::memory_order_relaxed);
    }

    /*
     * Signals the loop to stop, discards queued commands, runs the teardown
     * hook on the worker, and joins. Idempotent; safe on a never-started
     * worker and from any thread.
     */
    void stop_and_join() noexcept
    {
        std::lock_guard<std::mutex> lifecycle(lifecycle_);
        std::thread thread;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
            queue_.clear();
            condition_.notify_all();
            if (thread_.joinable()) {
                thread = std::move(thread_);
            }
        }
        if (thread.joinable()) {
            thread.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::stopped;
        running_.store(false, std::memory_order_release);
        worker_id_.store(std::thread::id{}, std::memory_order_relaxed);
        condition_.notify_all();
    }

private:
    enum class State {
        stopped,
        starting,
        running,
        failed,
    };

    struct Command {
        std::uint64_t sequence = 0;
        std::function<void()> run;
    };

    void worker_main()
    {
        worker_id_.store(std::this_thread::get_id(), std::memory_order_relaxed);
        bool startup_ok = true;
        if (startup_hook_) {
            try {
                startup_ok = startup_hook_();
            } catch (...) {
                startup_ok = false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = startup_ok ? State::running : State::failed;
            running_.store(startup_ok, std::memory_order_release);
        }
        condition_.notify_all();

        if (startup_ok) {
            for (;;) {
                Command command;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    condition_.wait(lock, [this] { return !queue_.empty() || stop_requested_; });
                    if (stop_requested_) {
                        queue_.clear();
                        break;
                    }
                    command = std::move(queue_.front());
                    queue_.pop_front();
                }
                commands_executed_.fetch_add(1, std::memory_order_relaxed);
                try {
                    command.run();
                } catch (...) {
                    /* A command must never let an exception escape the worker. */
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    completed_sequence_ = command.sequence;
                }
                condition_.notify_all();
            }
        }

        running_.store(false, std::memory_order_release);
        if (teardown_hook_) {
            try {
                teardown_hook_();
            } catch (...) {
                /* Teardown is best-effort; the thread still ends cleanly. */
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            state_ = State::stopped;
        }
        condition_.notify_all();
    }

    mutable std::mutex mutex_;
    /* Serializes start()/stop_and_join() lifecycle transitions (never per-command). */
    mutable std::mutex lifecycle_;
    std::condition_variable condition_;
    std::thread thread_;
    std::deque<Command> queue_;
    std::function<bool()> startup_hook_;
    std::function<void()> teardown_hook_;
    std::uint64_t submitted_sequence_ = 0;
    std::uint64_t completed_sequence_ = 0;
    bool stop_requested_ = false;
    State state_ = State::stopped;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> commands_executed_{0};
    std::atomic<std::thread::id> worker_id_{};
};

}  // namespace cvforwin::camera::uvc

#endif /* CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_COMMAND_WORKER_H_ */

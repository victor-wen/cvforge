/*
 * Coalesced retention trigger.
 *
 * RetentionScheduler is a pure policy object over an injected steady clock. It
 * fires after retention_time_trigger_seconds since the prior completed run OR
 * retention_committed_capture_trigger committed captures, whichever comes
 * first. It never scans, sorts, or deletes anything: the caller owns the
 * background worker that performs retention and records completion with
 * on_retention_run().
 */

#ifndef CVFORWIN_SRC_ARTIFACTS_RETENTION_SCHEDULER_H_
#define CVFORWIN_SRC_ARTIFACTS_RETENTION_SCHEDULER_H_

#include <chrono>
#include <cstdint>

namespace cvforwin::artifacts {

constexpr std::chrono::seconds k_retention_time_trigger{60};
constexpr std::uint64_t k_retention_capture_trigger = 100;

class RetentionScheduler {
public:
    explicit RetentionScheduler(std::chrono::seconds time_trigger = k_retention_time_trigger,
                                std::uint64_t capture_trigger = k_retention_capture_trigger) noexcept
        : time_trigger_(time_trigger), capture_trigger_(capture_trigger)
    {
    }

    /* Records one committed capture; true when the capture trigger fires now. */
    bool on_capture_committed(std::chrono::steady_clock::time_point /*now*/) noexcept
    {
        ++committed_captures_;
        return capture_trigger_ != 0u && committed_captures_ >= capture_trigger_;
    }

    /* True when the time trigger has elapsed since the last completed run. */
    bool due(std::chrono::steady_clock::time_point now) const noexcept
    {
        return (now - last_run_) >= time_trigger_;
    }

    /* Records that a retention run completed and resets the capture counter. */
    void on_retention_run(std::chrono::steady_clock::time_point now) noexcept
    {
        last_run_ = now;
        committed_captures_ = 0u;
    }

    std::uint64_t committed_captures() const noexcept
    {
        return committed_captures_;
    }

private:
    std::chrono::seconds time_trigger_;
    std::uint64_t capture_trigger_ = k_retention_capture_trigger;
    std::chrono::steady_clock::time_point last_run_;
    std::uint64_t committed_captures_ = 0u;
};

}  // namespace cvforwin::artifacts

#endif /* CVFORWIN_SRC_ARTIFACTS_RETENTION_SCHEDULER_H_ */

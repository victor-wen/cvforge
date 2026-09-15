/*
 * Monotonic operation deadline.
 *
 * A Deadline is an absolute steady-clock instant computed once from a timeout,
 * so nested stages can only observe the time that remains. remaining()
 * saturates at zero: an expired deadline never yields a negative duration to
 * callers that sleep for min(delay, remaining()).
 */

#ifndef CVFORWIN_SRC_CORE_DEADLINE_H_
#define CVFORWIN_SRC_CORE_DEADLINE_H_

#include <chrono>
#include <cstdint>

namespace cvforwin::core {

class Deadline {
public:
    /* A deadline that expires timeout_ms from now; timeout_ms == 0 expires at once. */
    static Deadline from_timeout_ms(std::uint32_t timeout_ms) noexcept;

    /* An already expired deadline. */
    static Deadline immediate() noexcept;

    bool expired() const noexcept;

    /* Time left until the deadline, clamped to >= 0. */
    std::chrono::milliseconds remaining() const noexcept;

private:
    std::chrono::steady_clock::time_point deadline_;
};

}  // namespace cvforwin::core

#endif /* CVFORWIN_SRC_CORE_DEADLINE_H_ */

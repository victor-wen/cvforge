#include "core/deadline.h"

namespace cvforwin::core {

Deadline Deadline::from_timeout_ms(std::uint32_t timeout_ms) noexcept
{
    Deadline deadline;
    const auto timeout = std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(timeout_ms)};
    deadline.deadline_ = std::chrono::steady_clock::now() + timeout;
    return deadline;
}

Deadline Deadline::immediate() noexcept
{
    return Deadline{};
}

bool Deadline::expired() const noexcept
{
    return std::chrono::steady_clock::now() >= deadline_;
}

std::chrono::milliseconds Deadline::remaining() const noexcept
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline_) {
        return std::chrono::milliseconds::zero();
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now);
}

}  // namespace cvforwin::core

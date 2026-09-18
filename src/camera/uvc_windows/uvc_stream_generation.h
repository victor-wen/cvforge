/*
 * Portable per-stream generation gate for the Windows UVC backend.
 *
 * This header intentionally carries no _WIN32 guard and no Media Foundation,
 * OpenCV, or other platform dependency: it is the pure, deterministic part of
 * the UVC backend's callback-generation lifetime rule, so it is unit-testable
 * on the portable Linux host. The Windows-only translation unit
 * src/camera/uvc_windows/uvc_backend.cpp includes it and gives every started
 * SourceReader a fresh callback carrying the generation returned by
 * begin_stream(); a callback completion is applied only while the gate still
 * accepts that generation.
 *
 * One backend owns one gate for its whole lifetime, so generations never
 * restart: stream N always has a strictly greater generation than stream N-1,
 * across close, reconnect, and re-open. Generation 0 is the "no active stream"
 * sentinel and is never returned by begin_stream().
 *
 * The gate is internally synchronized (atomics): begin_stream()/retire() run
 * on the backend's owned worker thread while a free-threaded Media Foundation
 * callback may call accepts()/discard_stale() concurrently from a work-queue
 * thread and the worker may call active_generation()/stale_events_discarded()
 * from its diagnostics accessor. Equality of the generation and the active
 * slot is the only ordering that matters, so relaxed ordering is sufficient.
 */

#ifndef CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_STREAM_GENERATION_H_
#define CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_STREAM_GENERATION_H_

#include <atomic>
#include <cstdint>

namespace cvforwin::camera::uvc {

/* Monotonic stream generation gate. Generation 0 means "no active stream". */
class StreamGenerationGate {
public:
    /*
     * Starts a new stream: retires the current generation, returns the next
     * strictly greater generation (the first active generation is 1), and
     * marks it active.
     */
    std::uint64_t begin_stream() noexcept
    {
        const std::uint64_t generation = next_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
        active_generation_.store(generation, std::memory_order_relaxed);
        return generation;
    }

    /* Retires the active generation; later events are stale until the next begin_stream. */
    void retire() noexcept
    {
        active_generation_.store(k_no_generation, std::memory_order_relaxed);
    }

    /* True only when `generation` equals the active generation and a stream is active. */
    bool accepts(std::uint64_t generation) const noexcept
    {
        return generation != k_no_generation && generation == active_generation_.load(std::memory_order_relaxed);
    }

    /* Records an event that `accepts` rejected; returns the new stale count. */
    std::uint64_t discard_stale(std::uint64_t generation) noexcept
    {
        if (!accepts(generation)) {
            return stale_events_discarded_.fetch_add(1, std::memory_order_relaxed) + 1;
        }
        return stale_events_discarded_.load(std::memory_order_relaxed);
    }

    std::uint64_t active_generation() const noexcept
    {
        return active_generation_.load(std::memory_order_relaxed);
    }

    std::uint64_t stale_events_discarded() const noexcept
    {
        return stale_events_discarded_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::uint64_t k_no_generation = 0;
    /* Monotonic source of generations; independent of the active slot so a
     * begin_stream after retire continues strictly upward. */
    std::atomic<std::uint64_t> next_generation_{0};
    std::atomic<std::uint64_t> active_generation_{k_no_generation};
    std::atomic<std::uint64_t> stale_events_discarded_{0};
};

}  // namespace cvforwin::camera::uvc

#endif /* CVFORWIN_SRC_CAMERA_UVC_WINDOWS_UVC_STREAM_GENERATION_H_ */

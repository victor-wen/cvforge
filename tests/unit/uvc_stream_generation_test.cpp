/*
 * Developer-owned portable tests for the UVC stream-generation gate.
 *
 * The UVC backend gives every started SourceReader a fresh callback tagged
 * with a strictly increasing generation and applies a completion only while
 * the gate still accepts that generation. uvc_stream_generation.h is pure
 * logic with no Windows API, Media Foundation, or OpenCV dependency, so its
 * behavior is exercised here on the portable Linux host. This complements the
 * independent cvf103 portable gate suite and the Windows-only callback-snapshot
 * suite; it does not replace either.
 *
 * File name does not start with "cvf1": the independent suite owns that prefix.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "camera/uvc_windows/uvc_stream_generation.h"

namespace {

namespace uvc = cvforwin::camera::uvc;

using uvc::StreamGenerationGate;

constexpr std::uint64_t k_no_generation = 0;

}  // namespace

TEST_CASE("UVC developer: a fresh gate has no active stream", "[uvc][generation-developer]")
{
    StreamGenerationGate gate;

    CHECK(gate.active_generation() == k_no_generation);
    CHECK(gate.stale_events_discarded() == 0);
    CHECK_FALSE(gate.accepts(k_no_generation));
    CHECK_FALSE(gate.accepts(1));
}

TEST_CASE("UVC developer: begin_stream starts at one and increases strictly across retire", "[uvc][generation-developer]")
{
    StreamGenerationGate gate;

    const std::uint64_t first = gate.begin_stream();
    CHECK(first == 1);
    CHECK(gate.active_generation() == first);
    CHECK(gate.accepts(first));

    const std::uint64_t second = gate.begin_stream();
    CHECK(second == first + 1);
    CHECK(gate.accepts(second));
    CHECK_FALSE(gate.accepts(first));

    gate.retire();
    CHECK(gate.active_generation() == k_no_generation);
    CHECK_FALSE(gate.accepts(second));

    /* The sequence continues after retire; a retired generation is never reused. */
    const std::uint64_t replacement = gate.begin_stream();
    CHECK(replacement == second + 1);
    CHECK(gate.accepts(replacement));
    CHECK_FALSE(gate.accepts(second));
    CHECK_FALSE(gate.accepts(replacement + 1));
}

TEST_CASE("UVC developer: accepts is an exact equality against the active generation", "[uvc][generation-developer]")
{
    StreamGenerationGate gate;
    const std::uint64_t active = gate.begin_stream();

    CHECK(gate.accepts(active));
    CHECK_FALSE(gate.accepts(active - 1));
    CHECK_FALSE(gate.accepts(active + 1));

    /* Retire makes even the previously active generation unacceptable. */
    gate.retire();
    for (std::uint64_t candidate = active - 1; candidate <= active + 1; ++candidate) {
        CHECK_FALSE(gate.accepts(candidate));
    }
}

TEST_CASE("UVC developer: discard_stale counts only rejected generations", "[uvc][generation-developer]")
{
    StreamGenerationGate gate;

    const std::uint64_t stale = gate.begin_stream();
    const std::uint64_t active = gate.begin_stream();

    CHECK_FALSE(gate.accepts(stale));
    CHECK(gate.stale_events_discarded() == 0);

    CHECK(gate.discard_stale(stale) == 1);
    CHECK(gate.stale_events_discarded() == 1);
    CHECK(gate.discard_stale(stale) == 2);
    CHECK(gate.stale_events_discarded() == 2);

    /* An accepted generation is not a discard and does not change the counter. */
    CHECK(gate.discard_stale(active) == 2);
    CHECK(gate.stale_events_discarded() == 2);

    /* After retire the former active generation becomes stale as well. */
    gate.retire();
    CHECK(gate.discard_stale(active) == 3);
    CHECK(gate.stale_events_discarded() == 3);
}

TEST_CASE("UVC developer: accepting the active generation never marks it stale", "[uvc][generation-developer]")
{
    StreamGenerationGate gate;
    const std::uint64_t active = gate.begin_stream();

    for (int round = 0; round < 8; ++round) {
        CHECK(gate.accepts(active));
    }
    CHECK(gate.stale_events_discarded() == 0);
    CHECK(gate.active_generation() == active);
}

TEST_CASE("UVC developer: a late retired-generation completion is rejected after replacement",
          "[uvc][generation-developer]")
{
    StreamGenerationGate gate;

    /* Stream one is active, then a replacement begins before stream one's
     * delayed completion arrives: only the replacement is accepted. */
    const std::uint64_t stream_one = gate.begin_stream();
    const std::uint64_t stream_two = gate.begin_stream();

    CHECK_FALSE(gate.accepts(stream_one));
    CHECK(gate.accepts(stream_two));
    CHECK(gate.discard_stale(stream_one) == 1);
    CHECK(gate.stale_events_discarded() == 1);
}

TEST_CASE("UVC developer: concurrent begin/retire/accepts keep generations strictly increasing",
          "[uvc][generation-developer]")
{
    StreamGenerationGate gate;
    constexpr int k_rounds = 500;

    /* A single producer begins and retires streams while consumer threads poll
     * accepts()/active_generation(). Generations must stay strictly increasing
     * and the stale counter must never regress. */
    std::vector<std::uint64_t> observed;
    observed.reserve(k_rounds);
    std::atomic<bool> stop{false};

    std::thread poller([&gate, &stop] {
        while (!stop.load(std::memory_order_relaxed)) {
            const std::uint64_t active = gate.active_generation();
            if (active != k_no_generation) {
                (void)gate.accepts(active);
            }
            (void)gate.stale_events_discarded();
        }
    });

    std::uint64_t previous = 0;
    for (int round = 0; round < k_rounds; ++round) {
        const std::uint64_t generation = gate.begin_stream();
        CHECK(generation > previous);
        observed.push_back(generation);
        previous = generation;
        gate.retire();
    }
    stop.store(true, std::memory_order_relaxed);
    poller.join();

    REQUIRE(observed.size() == static_cast<std::size_t>(k_rounds));
    for (std::size_t index = 0; index < observed.size(); ++index) {
        CHECK(observed[index] == static_cast<std::uint64_t>(index) + 1);
    }
    /* The last begin_stream retired its generation, so no stream is active. */
    CHECK(gate.active_generation() == k_no_generation);
}

/*
 * CVF-103 independent black-box unit tests for the portable stream-generation
 * gate declared in src/camera/uvc_windows/uvc_stream_generation.h.
 *
 * Coverage: brief observable behavior B4 (strictly increasing generations; a
 * completion is accepted only for the active generation) plus the negative case
 * (two consecutive generations both delivering completions: only the active one
 * is accepted) and the generation-related boundary cases. The gate is pure
 * logic, so this file is fully portable and runs on the WSL author host.
 *
 * Only the frozen seam header is included. No production .cpp is read, no
 * backend, Media Foundation object, or device is touched, and no implementation
 * detail is assumed beyond the brief-frozen class surface:
 *   begin_stream() -> uint64 (first active generation 1, strictly increasing)
 *   retire() -> void
 *   accepts(gen) const -> bool
 *   discard_stale(gen) -> uint64
 *   active_generation() const
 *   stale_events_discarded() const
 *
 * The uint64 return value of discard_stale() is not pinned by the brief, so the
 * cases observe its effect through stale_events_discarded() instead of guessing
 * its return semantics.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>

#include "camera/uvc_windows/uvc_stream_generation.h"

namespace {

namespace uvc = cvforwin::camera::uvc;

using uvc::StreamGenerationGate;

/* Generation 0 is the "no active stream" sentinel: the first active stream is 1. */
constexpr std::uint64_t kNoGeneration = 0;

}  // namespace

/* ------------------------------------------------------------------------- */
/* B4: first begin_stream returns the first active generation.               */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: first begin_stream returns generation 1", "[cvf-103][B4][generation]")
{
    StreamGenerationGate gate;

    const std::uint64_t first = gate.begin_stream();
    CHECK(first == 1);
    CHECK(gate.active_generation() == first);
}

/* ------------------------------------------------------------------------- */
/* B4: generations strictly increase across begin_stream calls.              */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: consecutive begin_stream generations are strictly increasing", "[cvf-103][B4][generation]")
{
    StreamGenerationGate gate;

    std::uint64_t previous = gate.begin_stream();
    CHECK(previous == 1);

    for (int round = 0; round < 6; ++round) {
        const std::uint64_t current = gate.begin_stream();
        CHECK(current > previous);
        CHECK(gate.active_generation() == current);
        previous = current;
    }
}

/* ------------------------------------------------------------------------- */
/* B4: accepts() is true only for the active generation.                     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: accepts is true only for the active generation", "[cvf-103][B4][accepts]")
{
    StreamGenerationGate gate;

    const std::uint64_t active = gate.begin_stream();

    CHECK(gate.accepts(active));
    CHECK_FALSE(gate.accepts(active + 1));
    CHECK_FALSE(gate.accepts(active + 2));
    CHECK_FALSE(gate.accepts(kNoGeneration));

    /* Before any stream the sentinel generation is not active and is rejected. */
    StreamGenerationGate fresh;
    CHECK_FALSE(fresh.accepts(kNoGeneration));
    CHECK(fresh.active_generation() == kNoGeneration);
}

/* ------------------------------------------------------------------------- */
/* B4: after retire() nothing is accepted.                                   */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: after retire no generation is accepted", "[cvf-103][B4][retire]")
{
    StreamGenerationGate gate;

    const std::uint64_t active = gate.begin_stream();
    CHECK(gate.accepts(active));

    gate.retire();

    CHECK_FALSE(gate.accepts(active));
    CHECK_FALSE(gate.accepts(active + 1));
    CHECK_FALSE(gate.accepts(kNoGeneration));
}

TEST_CASE("CVF-103 B4: a begin_stream after retire continues the sequence and activates only the new generation",
          "[cvf-103][B4][retire]")
{
    StreamGenerationGate gate;

    const std::uint64_t retired = gate.begin_stream();
    gate.retire();

    const std::uint64_t replacement = gate.begin_stream();
    CHECK(replacement > retired);
    CHECK(gate.active_generation() == replacement);
    CHECK(gate.accepts(replacement));
    CHECK_FALSE(gate.accepts(retired));
}

/* ------------------------------------------------------------------------- */
/* Negative case: a stale generation is rejected; discard_stale counts it.   */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 negative: a stale generation is rejected and discard_stale increments the counter",
          "[cvf-103][negative][stale]")
{
    StreamGenerationGate gate;

    const std::uint64_t stale = gate.begin_stream();
    const std::uint64_t active = gate.begin_stream();
    REQUIRE(active > stale);

    CHECK_FALSE(gate.accepts(stale));
    CHECK(gate.accepts(active));

    const std::uint64_t before = gate.stale_events_discarded();
    (void)gate.discard_stale(stale);
    CHECK(gate.stale_events_discarded() == before + 1);

    /* Discarding the same stale generation again is still a discard. */
    (void)gate.discard_stale(stale);
    CHECK(gate.stale_events_discarded() == before + 2);
}

TEST_CASE("CVF-103 negative: two consecutive generations both delivering completions accept only the active one",
          "[cvf-103][negative][stale]")
{
    StreamGenerationGate gate;

    const std::uint64_t generation_one = gate.begin_stream();
    const std::uint64_t generation_two = gate.begin_stream();
    REQUIRE(generation_two > generation_one);

    /* Both "deliver": only the active generation may be accepted. */
    CHECK_FALSE(gate.accepts(generation_one));
    CHECK(gate.accepts(generation_two));

    const std::uint64_t rejected_before = gate.stale_events_discarded();
    (void)gate.discard_stale(generation_one);
    (void)gate.discard_stale(generation_two);

    /* Only the stale generation contributed exactly one discard. */
    CHECK(gate.stale_events_discarded() == rejected_before + 1);
}

/* ------------------------------------------------------------------------- */
/* B4: accepting the active generation never increments the stale counter.   */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: accepting the active generation does not increment the stale counter",
          "[cvf-103][B4][stale]")
{
    StreamGenerationGate gate;

    const std::uint64_t active = gate.begin_stream();
    const std::uint64_t before = gate.stale_events_discarded();

    for (int accepted = 0; accepted < 5; ++accepted) {
        CHECK(gate.accepts(active));
    }

    CHECK(gate.stale_events_discarded() == before);
    CHECK(gate.active_generation() == active);
}

/* ------------------------------------------------------------------------- */
/* B4: active_generation()/stale_events_discarded() stay consistent.         */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: active_generation and stale_events_discarded are consistent across the sequence",
          "[cvf-103][B4][consistency]")
{
    StreamGenerationGate gate;

    CHECK(gate.active_generation() == kNoGeneration);
    CHECK(gate.stale_events_discarded() == 0);

    std::uint64_t expected_active = gate.begin_stream();
    std::uint64_t expected_stale = 0;
    CHECK(gate.active_generation() == expected_active);

    for (int round = 0; round < 5; ++round) {
        const std::uint64_t previous = expected_active;
        expected_active = gate.begin_stream();

        CHECK(expected_active > previous);
        CHECK(gate.active_generation() == expected_active);

        CHECK_FALSE(gate.accepts(previous));
        (void)gate.discard_stale(previous);
        ++expected_stale;

        CHECK(gate.stale_events_discarded() == expected_stale);
    }

    /* A future generation is never accepted. */
    CHECK_FALSE(gate.accepts(gate.active_generation() + 1));
    CHECK(gate.stale_events_discarded() == expected_stale);
}

/* ------------------------------------------------------------------------- */
/* Boundary: the maximum generation value.                                    */
/*                                                                            */
/* The frozen seam exposes no way to seed a gate near the uint64 ceiling, so  */
/* a true 2^64 wrap cannot be reached from the public surface; the closest    */
/* requirement-derived boundary is the maximum representable generation,      */
/* which must never be accepted while inactive and must not overflow the      */
/* stale counter.                                                             */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 boundary: the maximum generation value is rejected while inactive and counted once when discarded",
          "[cvf-103][boundary][generation]")
{
    const std::uint64_t kMaxGeneration = std::numeric_limits<std::uint64_t>::max();
    StreamGenerationGate gate;

    /* A fresh gate has no active generation, so the maximum is not active. */
    CHECK_FALSE(gate.accepts(kMaxGeneration));

    const std::uint64_t active = gate.begin_stream();
    CHECK(active == 1);

    /* While generation 1 is active, the maximum is a stale/future generation. */
    CHECK_FALSE(gate.accepts(kMaxGeneration));
    CHECK(gate.active_generation() == active);

    const std::uint64_t before = gate.stale_events_discarded();
    (void)gate.discard_stale(kMaxGeneration);
    CHECK(gate.stale_events_discarded() == before + 1);

    /* Discarding a boundary value never disturbs the active generation. */
    CHECK(gate.active_generation() == active);
    CHECK(gate.accepts(active));
}

/* ------------------------------------------------------------------------- */
/* Boundary: discard_stale does not disturb the active generation.            */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 boundary: discard_stale of the active generation does not disturb it",
          "[cvf-103][boundary][stale]")
{
    StreamGenerationGate gate;

    const std::uint64_t active = gate.begin_stream();
    const std::uint64_t stale_before = gate.stale_events_discarded();

    (void)gate.discard_stale(active);

    CHECK(gate.active_generation() == active);
    CHECK(gate.accepts(active));
    CHECK(gate.stale_events_discarded() == stale_before);
}

/* ------------------------------------------------------------------------- */
/* Boundary: the zero sentinel is a stale generation.                         */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 boundary: the zero sentinel is a stale generation and never disturbs the active one",
          "[cvf-103][boundary][generation]")
{
    StreamGenerationGate gate;

    CHECK_FALSE(gate.accepts(kNoGeneration));

    const std::uint64_t active = gate.begin_stream();
    CHECK(gate.accepts(active));
    CHECK_FALSE(gate.accepts(kNoGeneration));

    const std::uint64_t before = gate.stale_events_discarded();
    (void)gate.discard_stale(kNoGeneration);
    CHECK(gate.stale_events_discarded() == before + 1);
    CHECK(gate.active_generation() == active);
    CHECK(gate.accepts(active));
}

/* ------------------------------------------------------------------------- */
/* Boundary: retire() then begin_stream() reuse.                              */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: retire then begin_stream activates only the replacement and rejects its neighbours",
          "[cvf-103][B4][retire][generation]")
{
    StreamGenerationGate gate;

    const std::uint64_t retired = gate.begin_stream();
    CHECK(gate.active_generation() == retired);

    gate.retire();
    CHECK_FALSE(gate.accepts(retired));

    const std::uint64_t replacement = gate.begin_stream();
    CHECK(replacement > retired);

    CHECK(gate.accepts(replacement));
    CHECK_FALSE(gate.accepts(retired));
    CHECK_FALSE(gate.accepts(replacement + 1));
    /* replacement - 1 is a real value distinct from the active generation. */
    CHECK_FALSE(gate.accepts(replacement - 1));

    /* Only the retired generation is stale, so only it is counted. */
    const std::uint64_t before = gate.stale_events_discarded();
    (void)gate.discard_stale(replacement);
    CHECK(gate.stale_events_discarded() == before);
    (void)gate.discard_stale(retired);
    CHECK(gate.stale_events_discarded() == before + 1);
}

/* ------------------------------------------------------------------------- */
/* B4: repeated retire() is idempotent.                                       */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 B4: repeated retire is idempotent and leaves no generation accepted",
          "[cvf-103][B4][retire]")
{
    StreamGenerationGate gate;

    const std::uint64_t retired = gate.begin_stream();
    gate.retire();
    gate.retire();

    CHECK_FALSE(gate.accepts(retired));
    CHECK_FALSE(gate.accepts(retired + 1));
    CHECK_FALSE(gate.accepts(kNoGeneration));

    const std::uint64_t replacement = gate.begin_stream();
    CHECK(replacement > retired);
    CHECK(gate.accepts(replacement));
    CHECK_FALSE(gate.accepts(retired));
}

/* ------------------------------------------------------------------------- */
/* Negative: three consecutive generations, only the newest is accepted.      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-103 negative: three consecutive generations accept only the newest and count only stale discards",
          "[cvf-103][negative][stale][generation]")
{
    StreamGenerationGate gate;

    const std::uint64_t generation_one = gate.begin_stream();
    const std::uint64_t generation_two = gate.begin_stream();
    const std::uint64_t generation_three = gate.begin_stream();
    REQUIRE(generation_one < generation_two);
    REQUIRE(generation_two < generation_three);

    CHECK_FALSE(gate.accepts(generation_one));
    CHECK_FALSE(gate.accepts(generation_two));
    CHECK(gate.accepts(generation_three));
    CHECK(gate.active_generation() == generation_three);

    const std::uint64_t before = gate.stale_events_discarded();
    (void)gate.discard_stale(generation_one);
    (void)gate.discard_stale(generation_two);
    (void)gate.discard_stale(generation_three);
    CHECK(gate.stale_events_discarded() == before + 2);
}

/*
 * CVF-104 independent black-box unit tests for the frozen retention scheduler
 * seam src/artifacts/retention_scheduler.h.
 *
 * Coverage: the frozen constants, the 60-second time trigger, the 100-committed-
 * capture trigger, "whichever comes first", coalescing (the scheduler never runs
 * retention and stays due until a completed run is recorded), and the capture
 * counter reset performed by on_retention_run. All time is injected through the
 * steady_clock time_point parameters, so the cases are deterministic and never
 * sleep.
 *
 * Only the frozen seam header is included. No production .cpp is read.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

#include "artifacts/retention_scheduler.h"

namespace {

namespace art = cvforwin::artifacts;

using Clock = std::chrono::steady_clock;

/* An arbitrary, fixed baseline instant used as the "prior run" time. */
const Clock::time_point kBase{std::chrono::seconds(1000)};

TEST_CASE("CVF-104 retention: the frozen scheduling constants are 60 seconds and 100 captures",
          "[cvf-104][retention][constants]")
{
    CHECK(art::k_retention_time_trigger == std::chrono::seconds(60));
    CHECK(std::chrono::duration_cast<std::chrono::seconds>(art::k_retention_time_trigger).count() ==
          60);
    CHECK(art::k_retention_capture_trigger == 100u);
}

TEST_CASE("CVF-104 retention: the time trigger fires at exactly 60 seconds after a completed run",
          "[cvf-104][retention][time]")
{
    art::RetentionScheduler scheduler;
    scheduler.on_retention_run(kBase);

    CHECK(scheduler.committed_captures() == 0u);
    CHECK_FALSE(scheduler.due(kBase));
    CHECK_FALSE(scheduler.due(kBase + std::chrono::seconds(59)));
    CHECK(scheduler.due(kBase + std::chrono::seconds(60)));
    CHECK(scheduler.due(kBase + std::chrono::seconds(61)));
}

TEST_CASE("CVF-104 retention: a completed run resets the time baseline",
          "[cvf-104][retention][time]")
{
    art::RetentionScheduler scheduler;
    scheduler.on_retention_run(kBase);
    CHECK(scheduler.due(kBase + std::chrono::seconds(60)));

    scheduler.on_retention_run(kBase + std::chrono::seconds(60));
    CHECK_FALSE(scheduler.due(kBase + std::chrono::seconds(60)));
    CHECK_FALSE(scheduler.due(kBase + std::chrono::seconds(119)));
    CHECK(scheduler.due(kBase + std::chrono::seconds(120)));
}

TEST_CASE("CVF-104 retention: the capture trigger fires on the 100th committed capture",
          "[cvf-104][retention][captures]")
{
    art::RetentionScheduler scheduler;
    scheduler.on_retention_run(kBase);

    for (std::uint64_t count = 1; count < 100; ++count) {
        INFO("capture " << count);
        CHECK_FALSE(scheduler.on_capture_committed(kBase));
        CHECK(scheduler.committed_captures() == count);
    }

    CHECK(scheduler.on_capture_committed(kBase));
    CHECK(scheduler.committed_captures() == 100u);
}

TEST_CASE("CVF-104 retention: on_retention_run resets the committed-capture counter",
          "[cvf-104][retention][captures]")
{
    art::RetentionScheduler scheduler;
    scheduler.on_retention_run(kBase);

    for (int count = 0; count < 100; ++count) {
        (void)scheduler.on_capture_committed(kBase);
    }
    REQUIRE(scheduler.committed_captures() == 100u);

    scheduler.on_retention_run(kBase + std::chrono::seconds(1));
    CHECK(scheduler.committed_captures() == 0u);

    /* After the reset the trigger needs a fresh 100 captures. */
    for (std::uint64_t count = 1; count < 100; ++count) {
        CHECK_FALSE(scheduler.on_capture_committed(kBase + std::chrono::seconds(1)));
    }
    CHECK(scheduler.committed_captures() == 99u);
    CHECK(scheduler.on_capture_committed(kBase + std::chrono::seconds(1)));
    CHECK(scheduler.committed_captures() == 100u);
}

TEST_CASE("CVF-104 retention: whichever trigger comes first fires",
          "[cvf-104][retention][coalesced]")
{
    SECTION("the time trigger fires before 100 captures have accumulated")
    {
        art::RetentionScheduler scheduler;
        scheduler.on_retention_run(kBase);

        for (int count = 0; count < 50; ++count) {
            (void)scheduler.on_capture_committed(kBase);
        }

        CHECK(scheduler.due(kBase + std::chrono::seconds(60)));
        CHECK(scheduler.committed_captures() < art::k_retention_capture_trigger);
    }

    SECTION("the capture trigger fires before 60 seconds have elapsed")
    {
        art::RetentionScheduler scheduler;
        scheduler.on_retention_run(kBase);

        bool fired = false;
        for (std::uint64_t count = 0; count < 100; ++count) {
            fired = scheduler.on_capture_committed(kBase + std::chrono::seconds(30));
        }

        CHECK(fired);
        CHECK_FALSE(scheduler.due(kBase + std::chrono::seconds(30)));
    }
}

TEST_CASE("CVF-104 retention: the scheduler stays due (coalesced) until a completed run is recorded",
          "[cvf-104][retention][coalesced]")
{
    art::RetentionScheduler scheduler;
    scheduler.on_retention_run(kBase);

    /* Repeated queries never run retention and never clear the due state. */
    CHECK(scheduler.due(kBase + std::chrono::seconds(60)));
    CHECK(scheduler.due(kBase + std::chrono::seconds(60)));
    CHECK(scheduler.due(kBase + std::chrono::seconds(61)));

    /* A completed run is the only thing that clears it. */
    scheduler.on_retention_run(kBase + std::chrono::seconds(61));
    CHECK_FALSE(scheduler.due(kBase + std::chrono::seconds(61)));
}

TEST_CASE("CVF-104 retention: custom triggers replace the defaults",
          "[cvf-104][retention][constants]")
{
    art::RetentionScheduler scheduler(std::chrono::seconds(5), 3u);

    scheduler.on_retention_run(kBase);
    CHECK_FALSE(scheduler.due(kBase + std::chrono::seconds(4)));
    CHECK(scheduler.due(kBase + std::chrono::seconds(5)));

    scheduler.on_retention_run(kBase + std::chrono::seconds(5));
    CHECK_FALSE(scheduler.on_capture_committed(kBase));
    CHECK_FALSE(scheduler.on_capture_committed(kBase));
    CHECK(scheduler.on_capture_committed(kBase));
    CHECK(scheduler.committed_captures() == 3u);

    scheduler.on_retention_run(kBase + std::chrono::seconds(5));
    CHECK(scheduler.committed_captures() == 0u);
}

/* ------------------------------------------------------------------------- */
/* Verify-phase additions (2026-09-19, owner: test-engineer).                 */
/* Requirement-derived edge cases from the CVF-104 brief. No existing         */
/* assertion changed.                                                         */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-104 retention boundary: the 99th capture is not due and the 100th is",
          "[cvf-104][retention][captures][boundary]")
{
    art::RetentionScheduler scheduler;
    scheduler.on_retention_run(kBase);

    for (std::uint64_t count = 0; count < 99; ++count) {
        (void)scheduler.on_capture_committed(kBase);
    }
    /* Exactly 99 committed captures: still one short of the trigger. */
    CHECK(scheduler.committed_captures() == 99u);
    CHECK_FALSE(scheduler.due(kBase));

    /* The 100th capture is the capture-trigger boundary and returns true; the
     * time trigger is independent and has not elapsed. */
    CHECK(scheduler.on_capture_committed(kBase));
    CHECK(scheduler.committed_captures() == 100u);
    CHECK_FALSE(scheduler.due(kBase));
}

TEST_CASE("CVF-104 retention boundary: due() is false one millisecond before 60s and true exactly at "
          "60s",
          "[cvf-104][retention][time][boundary]")
{
    art::RetentionScheduler scheduler;
    scheduler.on_retention_run(kBase);

    CHECK_FALSE(scheduler.due(kBase + std::chrono::milliseconds(59999)));
    CHECK(scheduler.due(kBase + std::chrono::seconds(60)));
    /* Neither boundary point required an on_capture_committed. */
    CHECK(scheduler.committed_captures() == 0u);
}

}  // namespace

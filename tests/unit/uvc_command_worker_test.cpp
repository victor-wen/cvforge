/*
 * Developer-owned portable tests for the UVC command marshaller.
 *
 * src/camera/uvc_windows/uvc_command_worker.h holds the pure threading
 * machinery that the Windows UVC backend uses to own one long-lived worker
 * thread. It has no Windows API, Media Foundation, or OpenCV dependency, so
 * this suite builds and runs on the portable Linux host and covers the
 * lifecycle, bounded marshalling, and balanced startup/teardown semantics that
 * the Windows-only backend relies on. The Windows-only backend behavior
 * (CoInitializeEx/MFStartup on the worker, host-apartment independence) is
 * covered separately by the Windows-gated developer test, never faked here.
 *
 * File name does not start with "cvf1": the independent suite owns that prefix.
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "camera/uvc_windows/uvc_command_worker.h"
#include "core/deadline.h"

namespace {

using cvforwin::camera::uvc::CommandWorker;
using cvforwin::core::Deadline;

}  // namespace

TEST_CASE("CommandWorker runs startup and teardown on the owned worker", "[uvc][worker-developer]")
{
    CommandWorker worker;
    std::atomic<std::thread::id> startup_thread{};
    std::atomic<std::thread::id> teardown_thread{};
    std::atomic<int> teardown_calls{0};

    const std::thread::id caller = std::this_thread::get_id();
    const bool started = worker.start(Deadline::from_timeout_ms(5000), [&] {
        startup_thread.store(std::this_thread::get_id());
        return true; }, [&] {
        teardown_thread.store(std::this_thread::get_id());
        teardown_calls.fetch_add(1); });

    REQUIRE(started);
    CHECK(worker.running());
    CHECK(startup_thread.load() != caller);
    CHECK(worker.worker_id() != caller);

    worker.stop_and_join();

    CHECK_FALSE(worker.running());
    CHECK(teardown_calls.load() == 1);
    CHECK(teardown_thread.load() == startup_thread.load());
}

TEST_CASE("CommandWorker executes submitted commands in order on the worker thread", "[uvc][worker-developer]")
{
    CommandWorker worker;
    REQUIRE(worker.start(Deadline::from_timeout_ms(5000), [] { return true; }, [] {}));

    const std::thread::id caller = std::this_thread::get_id();
    std::vector<int> observed;
    std::atomic<bool> ran_off_worker{false};

    for (int value = 0; value < 8; ++value) {
        const auto state = worker.submit(Deadline::from_timeout_ms(5000), [&observed, &ran_off_worker, caller, value] {
            if (std::this_thread::get_id() == caller) {
                ran_off_worker.store(true);
            }
            observed.push_back(value);
        });
        REQUIRE(state == CommandWorker::SubmitState::completed);
    }

    CHECK_FALSE(ran_off_worker.load());
    CHECK(observed == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7});
    CHECK(worker.commands_executed() == 8);
    CHECK(caller != worker.worker_id());

    worker.stop_and_join();
}

TEST_CASE("CommandWorker reports an expired deadline without executing the command", "[uvc][worker-developer]")
{
    CommandWorker worker;
    REQUIRE(worker.start(Deadline::from_timeout_ms(5000), [] { return true; }, [] {}));

    std::atomic<bool> executed{false};
    const auto state = worker.submit(Deadline::immediate(), [&executed] { executed.store(true); });

    CHECK(state == CommandWorker::SubmitState::timeout);
    CHECK_FALSE(executed.load());
    CHECK(worker.commands_executed() == 0);

    worker.stop_and_join();
}

TEST_CASE("CommandWorker times out a command that exceeds the deadline but still completes it", "[uvc][worker-developer]")
{
    CommandWorker worker;
    REQUIRE(worker.start(Deadline::from_timeout_ms(5000), [] { return true; }, [] {}));

    std::atomic<bool> released{false};
    std::atomic<bool> finished{false};
    const auto state = worker.submit(Deadline::from_timeout_ms(50), [&released, &finished] {
        /* Spin until the test releases it; this models a long Media Foundation call. */
        while (!released.load()) {
            std::this_thread::yield();
        }
        finished.store(true);
    });

    CHECK(state == CommandWorker::SubmitState::timeout);

    released.store(true);
    const auto drain = worker.submit(Deadline::from_timeout_ms(5000), [] {});
    CHECK(drain == CommandWorker::SubmitState::completed);
    CHECK(finished.load());
    CHECK(worker.commands_executed() == 2);

    worker.stop_and_join();
}

TEST_CASE("CommandWorker runs teardown exactly once even when startup fails", "[uvc][worker-developer]")
{
    CommandWorker worker;
    std::atomic<int> startup_calls{0};
    std::atomic<int> teardown_calls{0};

    const bool started = worker.start(Deadline::from_timeout_ms(5000), [&] {
        startup_calls.fetch_add(1);
        return false; }, [&] { teardown_calls.fetch_add(1); });

    CHECK_FALSE(started);
    CHECK_FALSE(worker.running());
    CHECK(startup_calls.load() == 1);
    CHECK(teardown_calls.load() == 1);
}

TEST_CASE("CommandWorker stop_and_join is idempotent and rejects commands afterwards", "[uvc][worker-developer]")
{
    CommandWorker worker;
    std::atomic<int> teardown_calls{0};
    REQUIRE(worker.start(Deadline::from_timeout_ms(5000), [] { return true; }, [&] { teardown_calls.fetch_add(1); }));

    worker.stop_and_join();
    worker.stop_and_join();
    worker.stop_and_join();

    CHECK(teardown_calls.load() == 1);
    CHECK_FALSE(worker.running());

    std::atomic<bool> executed{false};
    const auto state = worker.submit(Deadline::from_timeout_ms(5000), [&executed] { executed.store(true); });
    CHECK(state == CommandWorker::SubmitState::unavailable);
    CHECK_FALSE(executed.load());

    /* A never-started worker is also safe to close repeatedly. */
    CommandWorker fresh;
    fresh.stop_and_join();
    fresh.stop_and_join();
    CHECK_FALSE(fresh.running());
    CHECK(fresh.commands_executed() == 0);
}

TEST_CASE("CommandWorker can be started again after a stop", "[uvc][worker-developer]")
{
    CommandWorker worker;
    REQUIRE(worker.start(Deadline::from_timeout_ms(5000), [] { return true; }, [] {}));
    worker.submit(Deadline::from_timeout_ms(5000), [] {});
    CHECK(worker.commands_executed() == 1);
    worker.stop_and_join();

    REQUIRE(worker.start(Deadline::from_timeout_ms(5000), [] { return true; }, [] {}));
    CHECK(worker.running());
    /* Counters are per-worker-lifetime, so a fresh start resets them. Thread ids
     * may legitimately be reused by the platform and are not asserted unique. */
    CHECK(worker.commands_executed() == 0);
    CHECK(worker.worker_id() != std::thread::id{});
    worker.stop_and_join();
}

TEST_CASE("CommandWorker accepts concurrent submissions without losing work", "[uvc][worker-developer]")
{
    CommandWorker worker;
    REQUIRE(worker.start(Deadline::from_timeout_ms(5000), [] { return true; }, [] {}));

    constexpr int kThreads = 4;
    constexpr int kPerThread = 25;
    std::mutex counted_mutex;
    int counted = 0;
    std::vector<std::thread> submitters;
    submitters.reserve(kThreads);
    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        submitters.emplace_back([&worker, &counted_mutex, &counted] {
            for (int command_index = 0; command_index < kPerThread; ++command_index) {
                const auto state = worker.submit(Deadline::from_timeout_ms(5000), [&counted_mutex, &counted] {
                    std::lock_guard<std::mutex> lock(counted_mutex);
                    ++counted;
                });
                REQUIRE(state == CommandWorker::SubmitState::completed);
            }
        });
    }
    for (std::thread& submitter : submitters) {
        submitter.join();
    }

    CHECK(counted == kThreads * kPerThread);
    CHECK(worker.commands_executed() == static_cast<std::uint64_t>(kThreads * kPerThread));

    worker.stop_and_join();
}

// CVF-105 independent black-box tests: race-free diagnostics warning
// accumulation and observation (FR-026).
//
// Brief B5: concurrent warning writers and readers must not lose a warning bit
// and must not data-race. The public surface is Diagnostics::warnings() and
// Diagnostics::clear_warnings() (signatures unchanged) with a contained
// callback failure setting warning_log_sink_failed. The suite is deterministic
// (bounded loops, join-based synchronization, no sleeps) and is intended to run
// under the sanitizer preset.
//
// Production surface used (frozen header only): src/diagnostics/diagnostics.h
// and src/core/warnings.h; no production .cpp is read.

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "cvf105_test.helpers.h"

#include "core/warnings.h"
#include "diagnostics/diagnostics.h"
#include "runtime/finalization.h"

namespace {

namespace core = cvforwin::core;
namespace diag = cvforwin::diagnostics;
namespace rt = cvforwin::runtime;

std::atomic<unsigned long long> g_callback_calls{0};

void throwing_callback(std::uint32_t /*level*/, const char* /*message*/, void* /*user_data*/)
{
    g_callback_calls.fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("cvf105 contained callback failure");
}

diag::CallbackBinding throwing_binding()
{
    diag::CallbackBinding binding;
    binding.fn = &throwing_callback;
    binding.user_data = nullptr;
    return binding;
}

diag::DiagnosticsConfig diagnostics_config(const std::filesystem::path& log_dir)
{
    diag::DiagnosticsConfig config;
    config.level = core::LogLevel::info;
    config.max_file_bytes = 1048576;
    config.max_files = 2;
    config.log_dir = log_dir;
    // Callback-only operation keeps the concurrency case bounded and free of
    // file-sink I/O while still delivering the contained failure.
    config.enable_file_sink = false;
    return config;
}

std::unique_ptr<diag::Diagnostics> make_diagnostics(const std::filesystem::path& log_dir)
{
    auto created = diag::Diagnostics::create(diagnostics_config(log_dir), throwing_binding());
    REQUIRE(created.has_value());
    return std::move(created.value());
}

constexpr std::uint32_t kDocumentedBits = static_cast<std::uint32_t>(
    core::WarningFlags::warning_log_sink_failed | core::WarningFlags::warning_image_save_failed);

bool has_log_sink_warning(std::uint32_t warnings)
{
    return (warnings & static_cast<std::uint32_t>(core::WarningFlags::warning_log_sink_failed)) != 0u;
}

bool only_documented_bits(std::uint32_t warnings)
{
    return (warnings & ~kDocumentedBits) == 0u;
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* Baseline: a fresh instance has no warnings and a contained failure sets it. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B5 baseline: a fresh instance has no warning bits and a contained failure sets "
          "only warning_log_sink_failed",
          "[cvf-105][B5][warnings]")
{
    cvf105::TempDir base("warnings_baseline");
    g_callback_calls.store(0, std::memory_order_relaxed);
    std::unique_ptr<diag::Diagnostics> diagnostics = make_diagnostics(base.path() / "logs");

    CHECK(diagnostics->warnings() == 0u);

    diagnostics->log(core::LogLevel::info, "cvf105 contained failure");

    CHECK(g_callback_calls.load(std::memory_order_relaxed) == 1u);
    CHECK(has_log_sink_warning(diagnostics->warnings()));
    CHECK(only_documented_bits(diagnostics->warnings()));

    diagnostics->clear_warnings();
    CHECK(diagnostics->warnings() == 0u);
}

/* ------------------------------------------------------------------------- */
/* B5: concurrent failing writers and warning readers lose no bit.            */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B5: concurrent warning writers and readers lose no warning bit",
          "[cvf-105][B5][warnings][concurrency]")
{
    cvf105::TempDir base("warnings_writers");
    std::unique_ptr<diag::Diagnostics> diagnostics = make_diagnostics(base.path() / "logs");
    g_callback_calls.store(0, std::memory_order_relaxed);

    constexpr int kWriterThreads = 4;
    constexpr int kLogsPerWriter = 1000;
    std::atomic<bool> writers_done{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<bool> saw_unknown_bit{false};

    std::vector<std::thread> readers;
    readers.reserve(3);
    for (int index = 0; index < 3; ++index) {
        readers.emplace_back([&] {
            while (!writers_done.load(std::memory_order_acquire)) {
                const std::uint32_t observed = diagnostics->warnings();
                if (!only_documented_bits(observed)) {
                    saw_unknown_bit.store(true, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
            }
        });
    }

    std::vector<std::thread> writers;
    writers.reserve(kWriterThreads);
    for (int index = 0; index < kWriterThreads; ++index) {
        writers.emplace_back([&diagnostics] {
            for (int entry = 0; entry < kLogsPerWriter; ++entry) {
                diagnostics->log(core::LogLevel::info, "cvf105 concurrent warning");
            }
        });
    }

    for (std::thread& writer : writers) {
        writer.join();
    }
    writers_done.store(true, std::memory_order_release);
    for (std::thread& reader : readers) {
        reader.join();
    }

    CHECK(g_callback_calls.load(std::memory_order_relaxed) ==
          static_cast<unsigned long long>(kWriterThreads) * static_cast<unsigned long long>(kLogsPerWriter));
    CHECK(reads.load(std::memory_order_relaxed) > 0u);
    CHECK_FALSE(saw_unknown_bit.load(std::memory_order_relaxed));
    CHECK(has_log_sink_warning(diagnostics->warnings()));
    CHECK(only_documented_bits(diagnostics->warnings()));
}

/* ------------------------------------------------------------------------- */
/* B5: concurrent clear_warnings() against failing writers does not corrupt   */
/* the accumulated bit, and a quiesced failure still sets it.                 */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B5: concurrent clear and failure writers leave a settable, documented bit",
          "[cvf-105][B5][warnings][concurrency]")
{
    cvf105::TempDir base("warnings_clear_race");
    std::unique_ptr<diag::Diagnostics> diagnostics = make_diagnostics(base.path() / "logs");

    constexpr int kLogsPerWriter = 2000;
    std::atomic<bool> writers_done{false};
    std::atomic<bool> saw_unknown_bit{false};

    std::thread writer([&diagnostics] {
        for (int entry = 0; entry < kLogsPerWriter; ++entry) {
            diagnostics->log(core::LogLevel::info, "cvf105 concurrent failure");
        }
    });

    std::thread clearer([&] {
        while (!writers_done.load(std::memory_order_acquire)) {
            diagnostics->clear_warnings();
            if (!only_documented_bits(diagnostics->warnings())) {
                saw_unknown_bit.store(true, std::memory_order_relaxed);
            }
            std::this_thread::yield();
        }
    });

    writer.join();
    writers_done.store(true, std::memory_order_release);
    clearer.join();

    CHECK_FALSE(saw_unknown_bit.load(std::memory_order_relaxed));

    // After all concurrency has stopped, a single contained failure must still
    // set the documented bit: the clear/write interleaving lost no state.
    diagnostics->clear_warnings();
    diagnostics->log(core::LogLevel::info, "cvf105 quiesced failure");
    CHECK(has_log_sink_warning(diagnostics->warnings()));
    CHECK(only_documented_bits(diagnostics->warnings()));
}

/* ------------------------------------------------------------------------- */
/* B5 negative: only documented bits are ever set; a dropped entry sets none.  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B5 negative: no unknown warning bit is invented and a dropped entry sets none",
          "[cvf-105][B5][warnings][negative]")
{
    cvf105::TempDir base("warnings_bits");
    std::unique_ptr<diag::Diagnostics> diagnostics = make_diagnostics(base.path() / "logs");

    CHECK(only_documented_bits(diagnostics->warnings()));

    diagnostics->log(core::LogLevel::info, "cvf105 one failure");
    CHECK(only_documented_bits(diagnostics->warnings()));
    CHECK(has_log_sink_warning(diagnostics->warnings()));

    diagnostics->clear_warnings();
    // Below the configured info level: dropped, so no callback and no bit.
    diagnostics->log(core::LogLevel::trace, "cvf105 dropped entry");
    CHECK(diagnostics->warnings() == 0u);
    CHECK(only_documented_bits(diagnostics->warnings()));
}

/* ------------------------------------------------------------------------- */
/* B5 edge: the two documented bits are combined without loss under           */
/* interleaved concurrent derivation.                                         */
/*                                                                            */
/* Diagnostics exposes only one bit directly, so the two distinct documented  */
/* bits meet at the frozen post-frame finalization seam: the log-sink bit is  */
/* passed in as warn_base and the image-save bit is added by an optional save  */
/* that was attempted but did not commit. Every decision, across interleaved  */
/* concurrent derivations, must still carry both bits and no unknown bit.     */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B5 edge: interleaved log-sink and image-save bits never lose either",
          "[cvf-105][B5][warnings][concurrency]")
{
    const std::uint32_t log_bit =
        static_cast<std::uint32_t>(core::WarningFlags::warning_log_sink_failed);
    const std::uint32_t image_bit =
        static_cast<std::uint32_t>(core::WarningFlags::warning_image_save_failed);
    const std::uint32_t both_bits = log_bit | image_bit;

    rt::FinalizeRequest request;
    request.verdict = core::Verdict::pass;
    request.execution_ok = true;
    request.frame_valid = true;
    request.post_frame_status = core::Status::ok;
    request.post_frame_error = core::ErrorCode::none;
    request.requirement = rt::SaveRequirement::optional;
    request.save_attempted = true;
    request.save_committed = false;  // optional save attempted but not committed
    request.save_failed = true;
    request.deadline_expired = false;

    std::atomic<bool> lost{false};
    std::atomic<std::uint64_t> observations{0u};
    std::vector<std::thread> workers;
    workers.reserve(8);
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&] {
            for (int iteration = 0; iteration < 2000; ++iteration) {
                const rt::FinalizeDecision decision = rt::finalize_after_frame(request, log_bit);
                if ((decision.warning_flags & both_bits) != both_bits) {
                    lost.store(true, std::memory_order_relaxed);
                }
                if ((decision.warning_flags & ~both_bits) != 0u) {
                    lost.store(true, std::memory_order_relaxed);
                }
                observations.fetch_add(1u, std::memory_order_relaxed);
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    CHECK_FALSE(lost.load(std::memory_order_relaxed));
    CHECK(observations.load(std::memory_order_relaxed) == 8u * 2000u);
}

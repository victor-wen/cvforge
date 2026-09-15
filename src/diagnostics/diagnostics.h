/*
 * Diagnostics fan-out.
 *
 * Diagnostics owns one optional size-rotating file sink below the configured
 * log directory and one optional synchronous C callback binding. A sink that
 * cannot be opened degrades the instance to callback-only operation instead of
 * failing the caller, and every contained failure is recorded as an internal
 * warning flag: optional diagnostics never change a valid inspection verdict.
 *
 * log() never throws. Messages are truncated to a UTF-8-safe prefix of at most
 * 4096 bytes before delivery, so a sink can never observe a split code point.
 */

#ifndef CVFORWIN_SRC_DIAGNOSTICS_DIAGNOSTICS_H_
#define CVFORWIN_SRC_DIAGNOSTICS_DIAGNOSTICS_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "core/result.h"
#include "core/status.h"
#include "core/warnings.h"

namespace cvforwin::diagnostics {

struct CallbackBinding {
    using Fn = void (*)(std::uint32_t level, const char* message_utf8, void* user_data);

    Fn fn = nullptr;
    void* user_data = nullptr;
};

struct DiagnosticsConfig {
    core::LogLevel level = core::LogLevel::info;
    std::uint64_t max_file_bytes = 10485760;
    std::uint32_t max_files = 5;
    std::filesystem::path log_dir;
    /* Optional file sink (CVF-006 additive field): false keeps callback-only operation. */
    bool enable_file_sink = true;
};

class Diagnostics {
public:
    /*
     * Validates the configuration, creates the log directory, and opens the
     * size-rotating file sink at <log_dir>/cvforwin.log when
     * config.enable_file_sink is true. A file sink that cannot be created opens
     * in degraded mode with warning_log_sink_failed. When enable_file_sink is
     * false no file is created and no warning is recorded for its absence.
     */
    static core::Result<std::unique_ptr<Diagnostics>> create(const DiagnosticsConfig& config,
                                                             CallbackBinding callback);

    /*
     * Writes one entry at or above the configured level to the active sinks,
     * synchronously on the caller thread. Below-level entries are dropped.
     */
    void log(core::LogLevel level, std::string_view message) noexcept;

    /* Accumulated internal warning bits (core::WarningFlags values). */
    std::uint32_t warnings() const noexcept;

    void clear_warnings() noexcept;

    /* Always <log_dir>/cvforwin.log, also in degraded mode. */
    std::filesystem::path log_file_path() const;

    ~Diagnostics();

    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;

private:
    struct State;

    Diagnostics(const DiagnosticsConfig& config, CallbackBinding callback);

    void open_file_sink() noexcept;

    std::unique_ptr<State> state_;
    std::filesystem::path log_file_path_;
    core::LogLevel level_ = core::LogLevel::info;
    CallbackBinding callback_;
    std::uint32_t warnings_ = core::warning_none;
};

}  // namespace cvforwin::diagnostics

#endif /* CVFORWIN_SRC_DIAGNOSTICS_DIAGNOSTICS_H_ */

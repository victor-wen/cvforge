#include "diagnostics/diagnostics.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <spdlog/common.h>
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "core/error.h"
#include "core/text.h"

namespace cvforwin::diagnostics {

namespace {

constexpr std::size_t k_max_message_bytes = 4096;
constexpr std::uint64_t k_min_file_bytes = 1024;
constexpr std::uint32_t k_min_files = 1;

spdlog::level::level_enum to_spdlog_level(core::LogLevel level) noexcept
{
    return static_cast<spdlog::level::level_enum>(core::to_public_log_level(level));
}

std::uint32_t warning_bit(core::WarningFlags flag) noexcept
{
    return static_cast<std::uint32_t>(flag);
}

}  // namespace

struct Diagnostics::State {
    std::uint64_t max_file_bytes = 0;
    std::uint32_t max_files = 0;
    bool file_sink_enabled = true;
    std::shared_ptr<spdlog::sinks::rotating_file_sink_mt> sink;
};

Diagnostics::Diagnostics(const DiagnosticsConfig& config, CallbackBinding callback)
    : state_(std::make_unique<State>()),
      log_file_path_(config.log_dir / "cvforwin.log"),
      level_(config.level),
      callback_(callback)
{
    state_->max_file_bytes = config.max_file_bytes;
    state_->max_files = config.max_files;
    state_->file_sink_enabled = config.enable_file_sink;
}

core::Result<std::unique_ptr<Diagnostics>> Diagnostics::create(const DiagnosticsConfig& config,
                                                               CallbackBinding callback)
{
    if (!config.log_dir.is_absolute()) {
        return core::invalid_argument(core::ErrorCode::path_not_absolute,
                                      "diagnostics log directory must be an absolute path");
    }
    if (config.max_file_bytes < k_min_file_bytes || config.max_files < k_min_files) {
        return core::invalid_argument(core::ErrorCode::diagnostics_invalid_config,
                                      "diagnostics max_file_bytes must be >= 1024 and max_files >= 1");
    }

    try {
        auto diagnostics = std::unique_ptr<Diagnostics>(new Diagnostics(config, callback));
        diagnostics->open_file_sink();
        return diagnostics;
    } catch (const std::exception&) {
        return core::make_failure(core::Status::internal_error, core::ErrorCode::internal_exception,
                                  "diagnostics initialization failed");
    }
}

void Diagnostics::open_file_sink() noexcept
{
    if (!state_->file_sink_enabled) {
        return;
    }

    try {
        std::error_code error;
        std::filesystem::create_directories(log_file_path_.parent_path(), error);
        if (error) {
            warnings_ |= warning_bit(core::warning_log_sink_failed);
            return;
        }

        /*
         * spdlog keeps max_files + 1 files (the current file plus max_files
         * rotated ones), so the rotated count is reduced by one to honor the
         * frozen bound that the cvforwin* family stays within max_files. With
         * max_files == 1 the sink truncates the current file on rotation.
         */
        const std::size_t rotated_files = state_->max_files > 0u
                                              ? static_cast<std::size_t>(state_->max_files - 1u)
                                              : std::size_t{0};
        state_->sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file_path_.string(), static_cast<std::size_t>(state_->max_file_bytes), rotated_files, false);
    } catch (...) {
        state_->sink.reset();
        warnings_ |= warning_bit(core::warning_log_sink_failed);
    }
}

void Diagnostics::log(core::LogLevel level, std::string_view message) noexcept
{
    if (core::to_public_log_level(level) < core::to_public_log_level(level_)) {
        return;
    }

    const std::string_view truncated = core::utf8_safe_prefix(message, k_max_message_bytes);

    if (state_->sink) {
        try {
            const spdlog::details::log_msg entry(spdlog::source_loc{}, "cvforwin", to_spdlog_level(level),
                                                 spdlog::string_view_t(truncated.data(), truncated.size()));
            state_->sink->log(entry);
        } catch (...) {
            warnings_ |= warning_bit(core::warning_log_sink_failed);
        }
    }

    if (callback_.fn != nullptr) {
        try {
            const std::string text{truncated};
            callback_.fn(core::to_public_log_level(level), text.c_str(), callback_.user_data);
        } catch (...) {
            warnings_ |= warning_bit(core::warning_log_sink_failed);
        }
    }
}

std::uint32_t Diagnostics::warnings() const noexcept
{
    return warnings_;
}

void Diagnostics::clear_warnings() noexcept
{
    warnings_ = core::warning_none;
}

std::filesystem::path Diagnostics::log_file_path() const
{
    return log_file_path_;
}

Diagnostics::~Diagnostics()
{
    if (!state_ || !state_->sink) {
        return;
    }
    try {
        state_->sink->flush();
    } catch (...) {
        warnings_ |= warning_bit(core::warning_log_sink_failed);
    }
}

}  // namespace cvforwin::diagnostics

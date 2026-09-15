#pragma once

// CVF-005 independent black-box test support (owner: test-engineer).
//
// Shared aliases, run-time temporary-directory helpers, callback recorders,
// deterministic frame builders, and file-system probes for the independent
// diagnostics and managed-capture suites. This header compiles only against the
// frozen interface headers listed in the CVF-005 test brief; it never includes
// production .cpp files and never inspects implementation state.
//
// Interface spellings used here are derived from the CVF-005 brief's frozen
// interface_reference; the small set of assumptions beyond that text is
// recorded in .ai/reports/CVF-005-test-red.yaml (author assumptions).
//
// Frozen interface headers first: a missing interface header must be the first
// diagnostic in the author (RED) phase.

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"
#include "core/warnings.h"

#include "artifacts/capture_store.h"
#include "diagnostics/diagnostics.h"

#include <catch2/catch_test_macros.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#endif

namespace cvf005 {

namespace core = cvforwin::core;
namespace diag = cvforwin::diagnostics;
namespace art = cvforwin::artifacts;

// --- run-time temporary directories ----------------------------------------

class TempDir {
public:
    explicit TempDir(std::string_view tag)
    {
        namespace fs = std::filesystem;
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        path_ = fs::temp_directory_path() / ("cvf005_" + std::string(tag) + "_" + unique);
        std::error_code error;
        fs::remove_all(path_, error);
        if (!fs::create_directories(path_, error)) {
            throw std::runtime_error("cvf005: cannot create temp directory " + path_.string());
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// --- callback probes --------------------------------------------------------
//
// The brief fixes the callback semantics (numeric level value, NUL-terminated
// message, pass-through user_data) but not the exact integral type of the level
// parameter, so these probes are captureless generic closures that convert to
// whatever numeric-level callback signature the frozen header declares. The
// levels themselves are asserted numerically (trace=0 .. critical=5).

struct CallbackRecord {
    std::atomic<std::uint32_t> calls{0};
    std::uint32_t last_level = 0;
    std::string last_message;
    std::size_t last_length = 0;
    std::vector<std::uint32_t> levels;
    std::vector<std::string> messages;
};

inline constexpr auto kRecordingCallback = [](auto level, const char* message, void* user_data) {
    auto* record = static_cast<CallbackRecord*>(user_data);
    ++record->calls;
    record->last_level = static_cast<std::uint32_t>(level);
    record->last_message = message != nullptr ? std::string(message) : std::string("<null>");
    record->last_length =
        message != nullptr ? static_cast<std::size_t>(std::strlen(message)) : std::size_t{0};
    record->levels.push_back(static_cast<std::uint32_t>(level));
    record->messages.push_back(record->last_message);
};

// C++ callback that throws: the diagnostics surface must contain the exception
// and turn it into warning_log_sink_failed without propagating.
inline constexpr auto kThrowingCallback = [](auto level, const char* message, void* user_data) {
    auto* record = static_cast<CallbackRecord*>(user_data);
    ++record->calls;
    record->last_level = static_cast<std::uint32_t>(level);
    record->last_message = message != nullptr ? std::string(message) : std::string("<null>");
    record->levels.push_back(static_cast<std::uint32_t>(level));
    throw std::runtime_error("cvf005 probe callback exception");
};

// --- diagnostics helpers ----------------------------------------------------

inline diag::DiagnosticsConfig diagnostics_config(core::LogLevel level,
                                                  std::filesystem::path log_dir,
                                                  std::uint64_t max_file_bytes = 10485760u,
                                                  std::uint32_t max_files = 5u)
{
    return diag::DiagnosticsConfig{
        .level = level,
        .max_file_bytes = max_file_bytes,
        .max_files = max_files,
        .log_dir = std::move(log_dir),
    };
}

inline diag::CallbackBinding recording_binding(CallbackRecord& record)
{
    return diag::CallbackBinding{.fn = kRecordingCallback, .user_data = &record};
}

inline diag::CallbackBinding throwing_binding(CallbackRecord& record)
{
    return diag::CallbackBinding{.fn = kThrowingCallback, .user_data = &record};
}

inline std::unique_ptr<diag::Diagnostics> create_diagnostics(const std::filesystem::path& log_dir,
                                                             core::LogLevel level,
                                                             CallbackRecord& record,
                                                             std::uint64_t max_file_bytes = 10485760u,
                                                             std::uint32_t max_files = 5u)
{
    auto created = diag::Diagnostics::create(diagnostics_config(level, log_dir, max_file_bytes,
                                                                max_files),
                                             recording_binding(record));
    REQUIRE(created.has_value());
    return std::move(created.value());
}

// Warning-flag helpers: the values are the brief's frozen bit values; the
// helpers stay agnostic about whether WarningFlags is scoped or unscoped.
template <typename Flags>
inline std::uint32_t warning_bits(Flags flags)
{
    return static_cast<std::uint32_t>(flags);
}

template <typename Flags>
inline bool has_warning(Flags flags, core::WarningFlags flag)
{
    return (warning_bits(flags) & static_cast<std::uint32_t>(flag)) != 0u;
}

// --- capture helpers --------------------------------------------------------

inline cv::Mat bgr_frame(int width, int height, unsigned char value = 200)
{
    return cv::Mat(height, width, CV_8UC3, cv::Scalar(value, value, value));
}

// The frozen CaptureSaveRequest holds non-owning string_view identifiers, so the
// identifier storage must outlive the request for the whole save() call. This
// holder owns both strings and builds the request from them. request() is
// lvalue-qualified on purpose so the corrected dangling pattern cannot return:
// calling it on a temporary holder does not compile. See A6 in
// .ai/reports/CVF-005-test-red-amendment.yaml.
class SaveRequestHolder {
public:
    SaveRequestHolder(const cv::Mat& pixels, std::string recipe_id, std::string request_id,
                      std::uint64_t sequence)
        : recipe_storage_(std::move(recipe_id)),
          request_storage_(std::move(request_id)),
          request_{.pixels = pixels,
                   .recipe_id = recipe_storage_,
                   .request_id = request_storage_,
                   .sequence = sequence}
    {}

    SaveRequestHolder(const SaveRequestHolder&) = delete;
    SaveRequestHolder& operator=(const SaveRequestHolder&) = delete;

    const art::CaptureSaveRequest& request() const& noexcept
    {
        return request_;
    }
    const art::CaptureSaveRequest& request() const&& = delete;

private:
    std::string recipe_storage_;
    std::string request_storage_;
    art::CaptureSaveRequest request_;
};

inline SaveRequestHolder save_request(const cv::Mat& pixels, std::string recipe_id,
                                      std::string request_id, std::uint64_t sequence)
{
    return SaveRequestHolder(pixels, std::move(recipe_id), std::move(request_id), sequence);
}

inline std::unique_ptr<art::CaptureStore> create_store(const std::filesystem::path& root)
{
    auto created = art::CaptureStore::create(root);
    REQUIRE(created.has_value());
    return std::move(created.value());
}

inline void check_png(const std::filesystem::path& file, int width, int height)
{
    INFO("decoding " << file.string());
    const cv::Mat decoded = cv::imread(file.string(), cv::IMREAD_UNCHANGED);
    REQUIRE_FALSE(decoded.empty());
    CHECK(decoded.cols == width);
    CHECK(decoded.rows == height);
    CHECK(decoded.channels() == 3);
}

inline void check_direct_child(const std::filesystem::path& root, const std::filesystem::path& child)
{
    CHECK(child.is_absolute());
    CHECK(child.parent_path() == root);
}

// --- file helpers -----------------------------------------------------------

inline void write_bytes(const std::filesystem::path& file, std::size_t bytes, char filler = 'x')
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << std::string(bytes, filler);
    REQUIRE(stream.good());
}

inline std::string read_bytes(const std::filesystem::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    REQUIRE(stream.good());
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

inline std::uintmax_t size_of_file(const std::filesystem::path& file)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    REQUIRE_FALSE(error);
    return size;
}

inline std::size_t count_files_with_prefix(const std::filesystem::path& dir,
                                           std::string_view prefix)
{
    std::size_t count = 0;
    std::error_code error;
    std::filesystem::directory_iterator iterator(dir, error);
    REQUIRE_FALSE(error);
    for (const auto& entry : iterator) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(prefix, 0) == 0u) {
            ++count;
        }
    }
    return count;
}

inline std::size_t count_files_with_extension(const std::filesystem::path& dir,
                                              std::string_view extension)
{
    std::size_t count = 0;
    std::error_code error;
    std::filesystem::directory_iterator iterator(dir, error);
    REQUIRE_FALSE(error);
    for (const auto& entry : iterator) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        if (entry.path().extension() == extension) {
            ++count;
        }
    }
    return count;
}

inline void set_age(const std::filesystem::path& file, std::chrono::seconds age)
{
    std::error_code error;
    std::filesystem::last_write_time(file,
                                     std::filesystem::file_time_type::clock::now() - age, error);
    REQUIRE_FALSE(error);
}

#ifndef _WIN32
// Ages the symlink itself (no-follow). std::filesystem::last_write_time follows
// the link, so the POSIX no-follow primitive is used to make the link itself
// eligible for age-based retention without touching its target.
inline bool set_symlink_age(const std::filesystem::path& link, std::chrono::seconds age)
{
    const auto stamp = std::chrono::system_clock::now() - age;
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(stamp.time_since_epoch()).count();
    struct timespec times[2]{};
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_sec = static_cast<time_t>(nanos / 1000000000LL);
    times[1].tv_nsec = static_cast<long>(nanos % 1000000000LL);
    return ::utimensat(AT_FDCWD, link.c_str(), times, AT_SYMLINK_NOFOLLOW) == 0;
}
#endif

// --- value checks -----------------------------------------------------------

inline void check_failure(const core::Failure& failure, core::Status status, core::ErrorCode code)
{
    CHECK(failure.status == status);
    CHECK(failure.code == code);
}

template <typename T>
inline void check_failure(const core::Result<T>& result, core::Status status, core::ErrorCode code)
{
    REQUIRE_FALSE(result.has_value());
    check_failure(result.failure(), status, code);
}

template <typename T>
inline void check_integer(const T& actual, std::int64_t expected, const char* what)
{
    INFO(what);
    CHECK(static_cast<std::int64_t>(actual) == expected);
}

// --- UTF-8 probe ------------------------------------------------------------

// Minimal well-formedness probe used to check that a truncated callback message
// never splits a code point.
inline bool valid_utf8(std::string_view text)
{
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80u) {
            ++index;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xE0u) == 0xC0u) {
            extra = 1;
            minimum = 0x80u;
        } else if ((lead & 0xF0u) == 0xE0u) {
            extra = 2;
            minimum = 0x800u;
        } else if ((lead & 0xF8u) == 0xF0u) {
            extra = 3;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (index + extra >= text.size()) {
            return false;
        }
        std::uint32_t code = lead & (0x7Fu >> extra);
        for (std::size_t step = 1; step <= extra; ++step) {
            const auto continuation = static_cast<unsigned char>(text[index + step]);
            if ((continuation & 0xC0u) != 0x80u) {
                return false;
            }
            code = (code << 6u) | (continuation & 0x3Fu);
        }
        if (code < minimum || code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
            return false;
        }
        index += extra + 1;
    }
    return true;
}

}  // namespace cvf005

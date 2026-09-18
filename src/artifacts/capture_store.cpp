#include "artifacts/capture_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include <atomic>

#include "core/error.h"

namespace cvforwin::artifacts {

namespace {

constexpr std::size_t k_max_identifier_bytes = 64;
constexpr std::string_view k_unnamed_identifier = "unnamed";
constexpr std::string_view k_png_extension = ".png";
constexpr std::string_view k_temporary_prefix = ".cvftmp_";

core::Failure artifacts_failure(core::ErrorCode code, std::string message)
{
    return core::make_failure(core::Status::internal_error, code, std::move(message));
}

core::Failure root_failure(std::string message)
{
    return core::make_failure(core::Status::config_error, core::ErrorCode::artifacts_root_error,
                              std::move(message));
}

core::Failure retention_failure(std::string message)
{
    return core::make_failure(core::Status::internal_error, core::ErrorCode::retention_error,
                              std::move(message));
}

bool is_identifier_byte(char character) noexcept
{
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-';
}

/* Sanitizes to [A-Za-z0-9._-], bounds to 64 bytes, and names empty ids "unnamed". */
std::string sanitize_identifier(std::string_view identifier)
{
    std::string sanitized;
    sanitized.reserve(std::min(identifier.size(), k_max_identifier_bytes));
    for (const char character : identifier) {
        if (sanitized.size() >= k_max_identifier_bytes) {
            break;
        }
        sanitized.push_back(is_identifier_byte(character) ? character : '_');
    }
    if (sanitized.empty()) {
        sanitized.assign(k_unnamed_identifier);
    }
    return sanitized;
}

std::string utc_stamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto since_epoch = now.time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count();
    const auto time_value = static_cast<std::time_t>(seconds);

    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &time_value);
#else
    gmtime_r(&time_value, &utc);
#endif

    const auto fraction = static_cast<unsigned>(((millis % 1000) + 1000) % 1000);
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%04d%02d%02dT%02d%02d%02d-%03u", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, fraction);
    return std::string(buffer.data());
}

std::string capture_filename(const CaptureSaveRequest& request)
{
    return utc_stamp() + "_" + sanitize_identifier(request.recipe_id) + "_" +
           sanitize_identifier(request.request_id) + "_" + std::to_string(request.sequence) + ".png";
}

core::Result<std::vector<unsigned char>> encode_png(const cv::Mat& pixels)
{
    std::vector<unsigned char> encoded;
    try {
        if (pixels.empty() ||
            !cv::imencode(std::string(k_png_extension), pixels, encoded) || encoded.empty()) {
            return artifacts_failure(core::ErrorCode::image_encode_error,
                                     "capture frame could not be encoded as PNG");
        }
    } catch (const std::exception&) {
        return artifacts_failure(core::ErrorCode::image_encode_error,
                                 "capture frame could not be encoded as PNG");
    }
    return encoded;
}

core::Result<void> write_png_file(const std::filesystem::path& target,
                                  const std::vector<unsigned char>& encoded)
{
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return artifacts_failure(core::ErrorCode::image_write_error,
                                 "capture PNG could not be written: " + target.string());
    }
    stream.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    stream.close();
    if (!stream) {
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        return artifacts_failure(core::ErrorCode::image_write_error,
                                 "capture PNG could not be written: " + target.string());
    }
    return core::Result<void>{};
}

/* Drops a trailing separator so the returned path's parent is the root itself. */
std::filesystem::path normalized_root(const std::filesystem::path& captures_root)
{
    if (captures_root.filename().empty() && captures_root.has_parent_path()) {
        return captures_root.parent_path();
    }
    return captures_root;
}

struct ScannedFile {
    std::filesystem::path path;
    std::uint64_t size;
    std::filesystem::file_time_type write_time;
};

bool older_first(const ScannedFile& left, const ScannedFile& right)
{
    if (left.write_time != right.write_time) {
        return left.write_time < right.write_time;
    }
    return left.path.string() < right.path.string();
}

}  // namespace

SaveDecision decide_capture_save(std::string_view save_policy, core::Verdict verdict, bool execution_ok)
{
    if (save_policy == "never") {
        return SaveDecision::skip;
    }
    if (save_policy == "always") {
        return SaveDecision::save;
    }
    if (save_policy == "fail_or_error") {
        return (!execution_ok || verdict == core::Verdict::fail) ? SaveDecision::save : SaveDecision::skip;
    }
    return SaveDecision::skip;
}

CaptureStore::CaptureStore(std::filesystem::path captures_root)
    : root_(std::move(captures_root))
{
}

core::Result<std::unique_ptr<CaptureStore>> CaptureStore::create(const std::filesystem::path& captures_root)
{
    if (!captures_root.is_absolute()) {
        return core::invalid_argument(core::ErrorCode::path_not_absolute,
                                      "captures root must be an absolute path");
    }

    std::error_code error;
    std::filesystem::create_directories(captures_root, error);
    if (error) {
        return root_failure("cannot create captures root: " + captures_root.string());
    }

    try {
        return std::unique_ptr<CaptureStore>(new CaptureStore(normalized_root(captures_root)));
    } catch (const std::exception&) {
        return core::make_failure(core::Status::internal_error, core::ErrorCode::internal_exception,
                                  "capture store initialization failed");
    }
}

core::Result<std::filesystem::path> CaptureStore::save(const CaptureSaveRequest& request)
{
    const std::filesystem::path target = root_ / capture_filename(request);

    core::Result<std::vector<unsigned char>> encoded = encode_png(request.pixels);
    if (!encoded.has_value()) {
        return encoded.failure();
    }
    core::Result<void> written = write_png_file(target, encoded.value());
    if (!written.has_value()) {
        return written.failure();
    }
    return target;
}

core::Result<std::filesystem::path> CaptureStore::save_temporary(const CaptureSaveRequest& request)
{
    static std::atomic<std::uint64_t> temporary_counter{0};
    const std::uint64_t serial = temporary_counter.fetch_add(1, std::memory_order_relaxed);
    const std::filesystem::path target =
        root_ / (std::string(k_temporary_prefix) + utc_stamp() + "_" + std::to_string(serial) + ".part");

    core::Result<std::vector<unsigned char>> encoded = encode_png(request.pixels);
    if (!encoded.has_value()) {
        return encoded.failure();
    }
    core::Result<void> written = write_png_file(target, encoded.value());
    if (!written.has_value()) {
        return written.failure();
    }
    return target;
}

core::Result<std::filesystem::path> CaptureStore::publish_temporary(
    const std::filesystem::path& temporary, const CaptureSaveRequest& request)
{
    const std::filesystem::path target = root_ / capture_filename(request);
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        return artifacts_failure(core::ErrorCode::image_write_error,
                                 "temporary capture could not be published: " + temporary.string());
    }
    return target;
}

bool CaptureStore::is_temporary_capture_name(std::string_view name) noexcept
{
    return name.size() >= k_temporary_prefix.size() &&
           name.compare(0, k_temporary_prefix.size(), k_temporary_prefix) == 0;
}

core::Result<std::uint32_t> CaptureStore::enforce_retention(const std::filesystem::path& captures_root,
                                                            std::chrono::seconds max_age,
                                                            std::uint64_t max_total_bytes)
{
    std::error_code error;
    std::filesystem::directory_iterator iterator(captures_root, std::filesystem::directory_options::none,
                                                 error);
    if (error) {
        return retention_failure("cannot scan captures root: " + captures_root.string());
    }

    std::vector<ScannedFile> files;
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const std::filesystem::directory_entry& entry = *iterator;
        const std::filesystem::file_status link_status = entry.symlink_status(error);
        if (error) {
            return retention_failure("cannot inspect captures entry: " + entry.path().string());
        }
        if (std::filesystem::is_regular_file(link_status)) {
            if (is_temporary_capture_name(entry.path().filename().string())) {
                iterator.increment(error);
                if (error) {
                    return retention_failure("cannot continue scanning captures root: " +
                                             captures_root.string());
                }
                continue;
            }
            const std::uintmax_t size = entry.file_size(error);
            if (error) {
                return retention_failure("cannot size captures entry: " + entry.path().string());
            }
            const std::filesystem::file_time_type write_time = entry.last_write_time(error);
            if (error) {
                return retention_failure("cannot timestamp captures entry: " + entry.path().string());
            }
            files.push_back(ScannedFile{entry.path(), static_cast<std::uint64_t>(size), write_time});
        }
        iterator.increment(error);
        if (error) {
            return retention_failure("cannot continue scanning captures root: " + captures_root.string());
        }
    }

    std::sort(files.begin(), files.end(), older_first);

    const auto now = std::filesystem::file_time_type::clock::now();
    std::vector<ScannedFile> kept;
    kept.reserve(files.size());
    std::uint64_t total_bytes = 0;
    std::uint32_t deleted = 0;

    for (const ScannedFile& file : files) {
        if (now - file.write_time > max_age) {
            std::filesystem::remove(file.path, error);
            if (error) {
                return retention_failure("cannot delete capture file: " + file.path.string());
            }
            ++deleted;
            continue;
        }
        total_bytes += file.size;
        kept.push_back(file);
    }

    std::sort(kept.begin(), kept.end(), older_first);
    for (const ScannedFile& file : kept) {
        if (total_bytes <= max_total_bytes) {
            break;
        }
        std::filesystem::remove(file.path, error);
        if (error) {
            return retention_failure("cannot delete capture file: " + file.path.string());
        }
        total_bytes = file.size <= total_bytes ? total_bytes - file.size : 0;
        ++deleted;
    }

    return deleted;
}

CaptureStore::~CaptureStore() = default;

}  // namespace cvforwin::artifacts

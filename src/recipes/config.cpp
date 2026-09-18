#include "recipes/config.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace cvforwin::recipes {

namespace {

using core::ErrorCode;
using core::Failure;
using core::Status;

constexpr std::size_t k_max_config_string_bytes = 256;

/*
 * Test-enabled builds (CVFORWIN_BUILD_TEST_BACKENDS=ON) additionally accept the
 * deterministic "file" and "synthetic" camera backends so WSL/CI can exercise
 * the full lifecycle without hardware. Release builds accept "uvc" only.
 */
#if defined(CVFORWIN_TEST_BACKENDS_ENABLED) && CVFORWIN_TEST_BACKENDS_ENABLED
constexpr bool k_test_backends_enabled = true;
#else
constexpr bool k_test_backends_enabled = false;
#endif
constexpr std::uint64_t k_min_frame_dimension = 1;
constexpr std::uint64_t k_max_frame_dimension = 16384;
constexpr double k_max_frame_rate = 1000.0;
constexpr std::uint64_t k_min_log_file_bytes = 1024;
constexpr std::uint64_t k_max_log_file_bytes = 1073741824;
constexpr std::uint64_t k_min_log_files = 1;
constexpr std::uint64_t k_max_log_files = 1000;
constexpr std::uint64_t k_min_retention_days = 1;
constexpr std::uint64_t k_max_retention_days = 3650;
constexpr std::uint64_t k_min_retention_bytes = 1048576;
constexpr std::uint64_t k_max_retention_bytes = 1099511627776;

Failure config_failure(ErrorCode code, std::string message)
{
    return core::make_failure(Status::config_error, code, std::move(message));
}

bool is_allowed_key(std::initializer_list<std::string_view> allowed, std::string_view key)
{
    return std::any_of(allowed.begin(), allowed.end(),
                       [key](std::string_view candidate) { return candidate == key; });
}

std::optional<Failure> reject_unknown_keys(const nlohmann::json& object,
                                           std::initializer_list<std::string_view> allowed,
                                           ErrorCode code)
{
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        if (!is_allowed_key(allowed, entry.key())) {
            return config_failure(code, "unknown configuration key: " + entry.key());
        }
    }
    return std::nullopt;
}

std::optional<Failure> require_keys(const nlohmann::json& object,
                                    std::initializer_list<std::string_view> required,
                                    ErrorCode code)
{
    for (const std::string_view key : required) {
        if (!object.contains(std::string{key})) {
            return config_failure(code, "missing configuration key: " + std::string{key});
        }
    }
    return std::nullopt;
}

bool read_unsigned(const nlohmann::json& value, std::uint64_t& out)
{
    if (value.is_number_unsigned()) {
        out = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {
        const auto signed_value = value.get<std::int64_t>();
        if (signed_value < 0) {
            return false;
        }
        out = static_cast<std::uint64_t>(signed_value);
        return true;
    }
    return false;
}

bool read_double(const nlohmann::json& value, double& out)
{
    if (!value.is_number()) {
        return false;
    }
    out = value.get<double>();
    return true;
}

bool read_string(const nlohmann::json& value, std::string& out)
{
    if (!value.is_string()) {
        return false;
    }
    out = value.get<std::string>();
    return true;
}

bool is_hex_digit(char character) noexcept
{
    return (character >= '0' && character <= '9') || (character >= 'A' && character <= 'F') ||
           (character >= 'a' && character <= 'f');
}

bool is_token(std::string_view value, std::initializer_list<std::string_view> allowed)
{
    return is_allowed_key(allowed, value);
}

std::optional<Failure> read_uint_field(const nlohmann::json& object, const char* key,
                                       std::uint64_t minimum, std::uint64_t maximum,
                                       std::uint64_t& out)
{
    std::uint64_t parsed = 0;
    if (!read_unsigned(object.at(key), parsed) || parsed < minimum || parsed > maximum) {
        return config_failure(ErrorCode::config_value_invalid,
                              std::string{"configuration value out of range: "} + key);
    }
    out = parsed;
    return std::nullopt;
}

std::optional<Failure> read_double_field(const nlohmann::json& object, const char* key,
                                         double minimum_exclusive, double maximum, double& out)
{
    double parsed = 0.0;
    if (!read_double(object.at(key), parsed) || !(parsed > minimum_exclusive) || parsed > maximum) {
        return config_failure(ErrorCode::config_value_invalid,
                              std::string{"configuration value out of range: "} + key);
    }
    out = parsed;
    return std::nullopt;
}

std::optional<Failure> read_string_field(const nlohmann::json& object, const char* key,
                                         std::string& out)
{
    if (!read_string(object.at(key), out)) {
        return config_failure(ErrorCode::config_value_invalid,
                              std::string{"configuration value must be text: "} + key);
    }
    return std::nullopt;
}

std::optional<Failure> read_optional_string_field(const nlohmann::json& object, const char* key,
                                                  std::string& out)
{
    const auto entry = object.find(key);
    if (entry == object.end()) {
        return std::nullopt;
    }
    if (!read_string(*entry, out)) {
        return config_failure(ErrorCode::config_value_invalid,
                              std::string{"configuration value must be text: "} + key);
    }
    return std::nullopt;
}

std::optional<Failure> validate_camera(const nlohmann::json& object, CameraSelectorConfig& out)
{
    if (!object.is_object()) {
        return config_failure(ErrorCode::config_value_invalid, "camera must be an object");
    }
    if (auto failure = reject_unknown_keys(
            object, {"backend", "device_path", "vendor_id", "product_id", "friendly_name"},
            ErrorCode::config_unknown_key)) {
        return failure;
    }
    if (!object.contains("backend")) {
        return config_failure(ErrorCode::config_value_invalid, "camera.backend is required");
    }
    if (auto failure = read_string_field(object, "backend", out.backend)) {
        return failure;
    }
    const bool is_test_backend = out.backend == "file" || out.backend == "synthetic";
    if (out.backend != "uvc" && !(is_test_backend && k_test_backends_enabled)) {
        return config_failure(ErrorCode::config_value_invalid,
                              k_test_backends_enabled ? "camera.backend must be uvc, file, or synthetic"
                                                      : "camera.backend must be uvc");
    }
    if (auto failure = read_optional_string_field(object, "device_path", out.device_path)) {
        return failure;
    }
    if (auto failure = read_optional_string_field(object, "vendor_id", out.vendor_id)) {
        return failure;
    }
    if (auto failure = read_optional_string_field(object, "product_id", out.product_id)) {
        return failure;
    }
    if (auto failure = read_optional_string_field(object, "friendly_name", out.friendly_name)) {
        return failure;
    }
    if (out.device_path.size() > k_max_config_string_bytes ||
        out.vendor_id.size() > k_max_config_string_bytes ||
        out.product_id.size() > k_max_config_string_bytes ||
        out.friendly_name.size() > k_max_config_string_bytes) {
        return config_failure(ErrorCode::config_value_invalid,
                              "camera selector strings must be at most 256 bytes");
    }
    if (!out.vendor_id.empty()) {
        auto canonical = canonicalize_hex4(out.vendor_id);
        if (!canonical.has_value()) {
            return canonical.failure();
        }
        out.vendor_id = std::move(canonical).value();
    }
    if (!out.product_id.empty()) {
        auto canonical = canonicalize_hex4(out.product_id);
        if (!canonical.has_value()) {
            return canonical.failure();
        }
        out.product_id = std::move(canonical).value();
    }
    if (out.device_path.empty() && (out.vendor_id.empty() || out.product_id.empty())) {
        return config_failure(ErrorCode::config_value_invalid,
                              "camera selector requires device_path or a VID/PID pair");
    }
    return std::nullopt;
}

std::optional<Failure> validate_base_capture(const nlohmann::json& object,
                                             CameraCaptureConfig& out)
{
    if (!object.is_object()) {
        return config_failure(ErrorCode::config_value_invalid, "base_capture must be an object");
    }
    if (auto failure = reject_unknown_keys(object, {"width", "height", "frame_rate", "pixel_format"},
                                           ErrorCode::config_unknown_key)) {
        return failure;
    }
    if (auto failure = require_keys(object, {"width", "height", "frame_rate", "pixel_format"},
                                    ErrorCode::config_value_invalid)) {
        return failure;
    }
    std::uint64_t width = 0;
    if (auto failure =
            read_uint_field(object, "width", k_min_frame_dimension, k_max_frame_dimension, width)) {
        return failure;
    }
    std::uint64_t height = 0;
    if (auto failure = read_uint_field(object, "height", k_min_frame_dimension,
                                       k_max_frame_dimension, height)) {
        return failure;
    }
    double frame_rate = 0.0;
    if (auto failure = read_double_field(object, "frame_rate", 0.0, k_max_frame_rate, frame_rate)) {
        return failure;
    }
    std::string pixel_format;
    if (auto failure = read_string_field(object, "pixel_format", pixel_format)) {
        return failure;
    }
    if (!is_token(pixel_format, {"any", "mono8", "bgr8", "rgb8"})) {
        return config_failure(ErrorCode::config_value_invalid,
                              "base_capture.pixel_format is not a supported token");
    }
    out.width = static_cast<std::uint32_t>(width);
    out.height = static_cast<std::uint32_t>(height);
    out.frame_rate = frame_rate;
    out.pixel_format = std::move(pixel_format);
    return std::nullopt;
}

std::optional<Failure> validate_logging(const nlohmann::json& object, LoggingConfig& out)
{
    if (!object.is_object()) {
        return config_failure(ErrorCode::config_value_invalid, "logging must be an object");
    }
    if (auto failure = reject_unknown_keys(object, {"level", "max_file_bytes", "max_files"},
                                           ErrorCode::config_unknown_key)) {
        return failure;
    }
    if (auto failure = require_keys(object, {"level", "max_file_bytes", "max_files"},
                                    ErrorCode::config_value_invalid)) {
        return failure;
    }
    std::string level;
    if (auto failure = read_string_field(object, "level", level)) {
        return failure;
    }
    if (!is_token(level, {"trace", "debug", "info", "warn", "error", "critical"})) {
        return config_failure(ErrorCode::config_value_invalid,
                              "logging.level is not a supported token");
    }
    std::uint64_t max_file_bytes = 0;
    if (auto failure = read_uint_field(object, "max_file_bytes", k_min_log_file_bytes,
                                       k_max_log_file_bytes, max_file_bytes)) {
        return failure;
    }
    std::uint64_t max_files = 0;
    if (auto failure =
            read_uint_field(object, "max_files", k_min_log_files, k_max_log_files, max_files)) {
        return failure;
    }
    out.level = std::move(level);
    out.max_file_bytes = max_file_bytes;
    out.max_files = static_cast<std::uint32_t>(max_files);
    return std::nullopt;
}

std::optional<Failure> validate_retention(const nlohmann::json& object, RetentionConfig& out)
{
    if (!object.is_object()) {
        return config_failure(ErrorCode::config_value_invalid, "retention must be an object");
    }
    if (auto failure = reject_unknown_keys(object, {"max_age_days", "max_total_bytes"},
                                           ErrorCode::config_unknown_key)) {
        return failure;
    }
    if (auto failure = require_keys(object, {"max_age_days", "max_total_bytes"},
                                    ErrorCode::config_value_invalid)) {
        return failure;
    }
    std::uint64_t max_age_days = 0;
    if (auto failure = read_uint_field(object, "max_age_days", k_min_retention_days,
                                       k_max_retention_days, max_age_days)) {
        return failure;
    }
    std::uint64_t max_total_bytes = 0;
    if (auto failure = read_uint_field(object, "max_total_bytes", k_min_retention_bytes,
                                       k_max_retention_bytes, max_total_bytes)) {
        return failure;
    }
    out.max_age_days = static_cast<std::uint32_t>(max_age_days);
    out.max_total_bytes = max_total_bytes;
    return std::nullopt;
}

}  // namespace

core::Result<std::string> canonicalize_hex4(std::string_view value)
{
    if (value.size() != 4u) {
        return config_failure(ErrorCode::config_value_invalid,
                              "camera vendor_id and product_id must be four hexadecimal characters");
    }
    std::string canonical{value};
    for (char& character : canonical) {
        if (!is_hex_digit(character)) {
            return config_failure(ErrorCode::config_value_invalid,
                                  "camera vendor_id and product_id must be four hexadecimal characters");
        }
        if (character >= 'A' && character <= 'F') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return canonical;
}

core::Result<GlobalConfig> load_global_config(const std::filesystem::path& config_root,
                                              const std::filesystem::path& output_root)
{
    if (!config_root.is_absolute() || !output_root.is_absolute()) {
        return core::invalid_argument(ErrorCode::path_not_absolute,
                                      "config_root and output_root must be absolute paths");
    }

    detail::JsonLoadResult document = detail::load_json_document(config_root / "cvforwin.json");
    switch (document.error) {
    case detail::JsonLoadError::file_missing:
        return config_failure(ErrorCode::config_file_missing, "cvforwin.json is missing");
    case detail::JsonLoadError::io_error:
        return config_failure(ErrorCode::config_io_error, "cvforwin.json cannot be read");
    case detail::JsonLoadError::parse_error:
        return config_failure(ErrorCode::config_parse_error, "cvforwin.json is not valid UTF-8 JSON");
    case detail::JsonLoadError::duplicate_key:
        return config_failure(ErrorCode::config_duplicate_key,
                              "cvforwin.json contains a duplicate key");
    case detail::JsonLoadError::none:
        break;
    }

    const nlohmann::json& root = document.value;
    if (!root.is_object()) {
        return config_failure(ErrorCode::config_value_invalid, "cvforwin.json root must be an object");
    }
    if (auto failure = reject_unknown_keys(
            root, {"schema_version", "camera", "base_capture", "logging", "retention"},
            ErrorCode::config_unknown_key)) {
        return *failure;
    }
    if (auto failure =
            require_keys(root, {"schema_version", "camera", "base_capture", "logging", "retention"},
                         ErrorCode::config_value_invalid)) {
        return *failure;
    }
    std::uint64_t schema_version = 0;
    if (!read_unsigned(root.at("schema_version"), schema_version) || schema_version != 1U) {
        return config_failure(ErrorCode::config_schema_version, "schema_version must be 1");
    }

    GlobalConfig config;
    config.schema_version = static_cast<std::uint32_t>(schema_version);
    if (auto failure = validate_camera(root.at("camera"), config.camera)) {
        return *failure;
    }
    if (auto failure = validate_base_capture(root.at("base_capture"), config.base_capture)) {
        return *failure;
    }
    if (auto failure = validate_logging(root.at("logging"), config.logging)) {
        return *failure;
    }
    if (auto failure = validate_retention(root.at("retention"), config.retention)) {
        return *failure;
    }
    config.config_root = config_root;
    config.output_root = output_root;
    return config;
}

namespace detail {

namespace {

/*
 * Strict duplicate-key detection for one JSON document.
 *
 * nlohmann's DOM parser silently keeps the last value of a duplicate key, so
 * the document is parsed once with this callback installed. The callback
 * tracks the key set of every open object: object keys are reported one depth
 * below their object_start event, so starting an object clears the set of the
 * next depth. Sibling objects (for example inside an array) therefore never
 * share a key set. Returning false discards the value and stops the callback
 * tree for that branch; the parse itself continues and the duplicate flag is
 * inspected afterwards.
 */
class DuplicateKeyCallback {
public:
    bool operator()(int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed)
    {
        if (event == nlohmann::json::parse_event_t::object_start) {
            keys_[depth + 1].clear();
        } else if (event == nlohmann::json::parse_event_t::key) {
            auto& keys = keys_[depth];
            if (!keys.insert(parsed.get<std::string>()).second) {
                duplicate_ = true;
                return false;
            }
        }
        return true;
    }

    bool duplicate() const noexcept
    {
        return duplicate_;
    }

private:
    std::map<int, std::set<std::string>> keys_;
    bool duplicate_ = false;
};

}  // namespace

JsonLoadResult load_json_document(const std::filesystem::path& path)
{
    JsonLoadResult result;
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        result.error = JsonLoadError::io_error;
        return result;
    }
    if (!exists) {
        result.error = JsonLoadError::file_missing;
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        result.error = JsonLoadError::io_error;
        return result;
    }
    const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    if (stream.bad()) {
        result.error = JsonLoadError::io_error;
        return result;
    }

    DuplicateKeyCallback callback;
    try {
        result.value = nlohmann::json::parse(
            text, [&callback](int depth, nlohmann::json::parse_event_t event,
                              nlohmann::json& parsed) { return callback(depth, event, parsed); });
    } catch (const nlohmann::json::exception&) {
        result.error = JsonLoadError::parse_error;
        return result;
    }
    if (callback.duplicate()) {
        result.error = JsonLoadError::duplicate_key;
    }
    return result;
}

}  // namespace detail

}  // namespace cvforwin::recipes

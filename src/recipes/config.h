/*
 * Global configuration contract (recipe_store module).
 *
 * load_global_config() strictly parses <config_root>/cvforwin.json: bounded,
 * UTF-8, versioned, duplicate-key-safe, and fully validated before any value
 * is returned. The document root and the artifact output root must both be
 * absolute; no relative path is accepted. Errors use the frozen recipe-store
 * numbering (config 1600-1607) and never escape as exceptions.
 */

#ifndef CVFORWIN_SRC_RECIPES_CONFIG_H_
#define CVFORWIN_SRC_RECIPES_CONFIG_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/result.h"

namespace cvforwin::recipes {

/*
 * Canonical form of a configured or enumerated VID/PID: exactly four lowercase
 * ASCII hex digits. Returns config_value_invalid for a null/empty value, non-hex
 * characters, or a length other than four. Identity comparison then runs on
 * canonical values only.
 */
core::Result<std::string> canonicalize_hex4(std::string_view value);

struct CameraSelectorConfig {
    std::string backend;
    std::string device_path;
    std::string vendor_id;
    std::string product_id;
    std::string friendly_name;
};

struct CameraCaptureConfig {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double frame_rate = 0.0;
    std::string pixel_format;
};

struct LoggingConfig {
    std::string level;
    std::uint64_t max_file_bytes = 0;
    std::uint32_t max_files = 0;
};

struct RetentionConfig {
    std::uint32_t max_age_days = 0;
    std::uint64_t max_total_bytes = 0;
};

struct GlobalConfig {
    std::uint32_t schema_version = 0;
    CameraSelectorConfig camera;
    CameraCaptureConfig base_capture;
    LoggingConfig logging;
    RetentionConfig retention;
    std::filesystem::path config_root;
    std::filesystem::path output_root;
};

/*
 * Reads and validates <config_root>/cvforwin.json and records both roots.
 * Missing/unreadable/duplicate/unknown/invalid documents fail with a
 * config_error failure whose code is one of the frozen 1600-1607 values;
 * a non-absolute root fails with invalid_argument/path_not_absolute.
 */
core::Result<GlobalConfig> load_global_config(const std::filesystem::path& config_root,
                                              const std::filesystem::path& output_root);

/*
 * Internal helper shared by the global-config and recipe loaders.
 *
 * Reads one JSON document as strict, duplicate-key-safe JSON. The helper is
 * deliberately not part of the frozen recipe interface: it exists so that the
 * duplicate-key rule has exactly one implementation in the module.
 */
namespace detail {

enum class JsonLoadError {
    none,
    file_missing,
    io_error,
    parse_error,
    duplicate_key,
};

struct JsonLoadResult {
    nlohmann::json value;
    JsonLoadError error = JsonLoadError::none;
};

JsonLoadResult load_json_document(const std::filesystem::path& path);

}  // namespace detail

}  // namespace cvforwin::recipes

#endif /* CVFORWIN_SRC_RECIPES_CONFIG_H_ */

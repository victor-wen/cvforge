#include "recipes/recipe.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "inspection/registry.h"
#include "recipes/config.h"

namespace cvforwin::recipes {

namespace {

using core::ErrorCode;
using core::Failure;
using core::Status;

constexpr std::size_t k_max_recipe_id_bytes = 128;
constexpr std::size_t k_max_algorithm_key_bytes = 64;
constexpr std::uint64_t k_max_settle_frames = 1000;
constexpr std::uint64_t k_min_frame_dimension = 1;
constexpr std::uint64_t k_max_frame_dimension = 16384;
constexpr double k_max_frame_rate = 1000.0;

Failure recipe_failure(ErrorCode code, std::string message)
{
    return core::make_failure(Status::config_error, code, std::move(message));
}

bool is_allowed_key(std::initializer_list<std::string_view> allowed, std::string_view key)
{
    return std::any_of(allowed.begin(), allowed.end(),
                       [key](std::string_view candidate) { return candidate == key; });
}

std::optional<Failure> reject_unknown_keys(const nlohmann::json& object,
                                           std::initializer_list<std::string_view> allowed)
{
    for (auto entry = object.begin(); entry != object.end(); ++entry) {
        if (!is_allowed_key(allowed, entry.key())) {
            return recipe_failure(ErrorCode::recipe_unknown_key,
                                  "unknown recipe key: " + entry.key());
        }
    }
    return std::nullopt;
}

std::optional<Failure> require_keys(const nlohmann::json& object,
                                    std::initializer_list<std::string_view> required)
{
    for (const std::string_view key : required) {
        if (!object.contains(std::string{key})) {
            return recipe_failure(ErrorCode::recipe_value_invalid,
                                  "missing recipe key: " + std::string{key});
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

bool is_token(std::string_view value, std::initializer_list<std::string_view> allowed)
{
    return is_allowed_key(allowed, value);
}

bool is_recipe_id_char(char character) noexcept
{
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '.' || character == '_' ||
           character == '-';
}

bool is_algorithm_key_char(char character) noexcept
{
    return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
           character == '.' || character == '_' || character == '-';
}

template <typename Predicate>
bool matches_byte_pattern(std::string_view value, std::size_t maximum_bytes,
                          Predicate predicate) noexcept
{
    return !value.empty() && value.size() <= maximum_bytes &&
           std::all_of(value.begin(), value.end(), predicate);
}

std::optional<Failure> validate_capture(const nlohmann::json& object, CaptureOverrides& out)
{
    if (!object.is_object()) {
        return recipe_failure(ErrorCode::recipe_capture_invalid, "capture must be an object");
    }
    if (auto failure = reject_unknown_keys(
            object, {"width", "height", "frame_rate", "pixel_format", "settle_frames"})) {
        return recipe_failure(ErrorCode::recipe_capture_invalid, failure->message);
    }
    if (!object.contains("settle_frames")) {
        return recipe_failure(ErrorCode::recipe_capture_invalid, "capture.settle_frames is required");
    }
    std::uint64_t settle_frames = 0;
    if (!read_unsigned(object.at("settle_frames"), settle_frames) ||
        settle_frames > k_max_settle_frames) {
        return recipe_failure(ErrorCode::recipe_capture_invalid,
                              "capture.settle_frames must be an integer in 0..1000");
    }
    out.settle_frames = static_cast<std::uint32_t>(settle_frames);

    if (const auto entry = object.find("width"); entry != object.end()) {
        std::uint64_t width = 0;
        if (!read_unsigned(*entry, width) || width < k_min_frame_dimension ||
            width > k_max_frame_dimension) {
            return recipe_failure(ErrorCode::recipe_capture_invalid,
                                  "capture.width must be an integer in 1..16384");
        }
        out.width = static_cast<std::uint32_t>(width);
    }
    if (const auto entry = object.find("height"); entry != object.end()) {
        std::uint64_t height = 0;
        if (!read_unsigned(*entry, height) || height < k_min_frame_dimension ||
            height > k_max_frame_dimension) {
            return recipe_failure(ErrorCode::recipe_capture_invalid,
                                  "capture.height must be an integer in 1..16384");
        }
        out.height = static_cast<std::uint32_t>(height);
    }
    if (const auto entry = object.find("frame_rate"); entry != object.end()) {
        double frame_rate = 0.0;
        if (!read_double(*entry, frame_rate) || !(frame_rate > 0.0) ||
            frame_rate > k_max_frame_rate) {
            return recipe_failure(ErrorCode::recipe_capture_invalid,
                                  "capture.frame_rate must be greater than 0 and at most 1000");
        }
        out.frame_rate = frame_rate;
    }
    if (const auto entry = object.find("pixel_format"); entry != object.end()) {
        std::string pixel_format;
        if (!read_string(*entry, pixel_format) ||
            !is_token(pixel_format, {"any", "mono8", "bgr8", "rgb8"})) {
            return recipe_failure(ErrorCode::recipe_capture_invalid,
                                  "capture.pixel_format is not a supported token");
        }
        out.pixel_format = std::move(pixel_format);
    }
    return std::nullopt;
}

std::optional<Failure> validate_artifacts(const nlohmann::json& object, ArtifactsConfig& out)
{
    if (!object.is_object()) {
        return recipe_failure(ErrorCode::recipe_artifacts_invalid, "artifacts must be an object");
    }
    if (auto failure = reject_unknown_keys(object, {"save_policy", "required"})) {
        return recipe_failure(ErrorCode::recipe_artifacts_invalid, failure->message);
    }
    if (!object.contains("save_policy") || !object.contains("required")) {
        return recipe_failure(ErrorCode::recipe_artifacts_invalid,
                              "artifacts.save_policy and artifacts.required are required");
    }
    std::string save_policy;
    if (!read_string(object.at("save_policy"), save_policy) ||
        !is_token(save_policy, {"always", "fail_or_error", "never"})) {
        return recipe_failure(ErrorCode::recipe_artifacts_invalid,
                              "artifacts.save_policy is not a supported token");
    }
    const nlohmann::json& required = object.at("required");
    if (!required.is_boolean()) {
        return recipe_failure(ErrorCode::recipe_artifacts_invalid,
                              "artifacts.required must be a boolean");
    }
    out.save_policy = std::move(save_policy);
    out.required = required.get<bool>();
    return std::nullopt;
}

}  // namespace

core::Result<Recipe> load_recipe_file(const std::filesystem::path& path,
                                      const inspection::AlgorithmRegistry& registry)
{
    detail::JsonLoadResult document = detail::load_json_document(path);
    switch (document.error) {
    case detail::JsonLoadError::file_missing:
        return recipe_failure(ErrorCode::recipe_file_missing, "recipe file is missing");
    case detail::JsonLoadError::io_error:
        return recipe_failure(ErrorCode::recipe_io_error, "recipe file cannot be read");
    case detail::JsonLoadError::parse_error:
        return recipe_failure(ErrorCode::recipe_parse_error,
                              "recipe file is not valid UTF-8 JSON");
    case detail::JsonLoadError::duplicate_key:
        return recipe_failure(ErrorCode::recipe_duplicate_key,
                              "recipe file contains a duplicate key");
    case detail::JsonLoadError::none:
        break;
    }

    const nlohmann::json& root = document.value;
    if (!root.is_object()) {
        return recipe_failure(ErrorCode::recipe_value_invalid, "recipe root must be an object");
    }
    if (auto failure = reject_unknown_keys(
            root, {"schema_version", "recipe_id", "algorithm", "parameters", "capture", "artifacts"})) {
        return *failure;
    }
    if (auto failure =
            require_keys(root, {"schema_version", "recipe_id", "algorithm", "parameters", "capture",
                                "artifacts"})) {
        return *failure;
    }

    std::uint64_t schema_version = 0;
    if (!read_unsigned(root.at("schema_version"), schema_version) || schema_version != 1U) {
        return recipe_failure(ErrorCode::recipe_schema_version, "schema_version must be 1");
    }

    std::string recipe_id;
    if (!read_string(root.at("recipe_id"), recipe_id) ||
        !matches_byte_pattern(recipe_id, k_max_recipe_id_bytes, is_recipe_id_char)) {
        return recipe_failure(ErrorCode::recipe_id_invalid,
                              "recipe_id must match [A-Za-z0-9._-]{1,128}");
    }

    std::string algorithm;
    if (!read_string(root.at("algorithm"), algorithm) ||
        !matches_byte_pattern(algorithm, k_max_algorithm_key_bytes, is_algorithm_key_char)) {
        return recipe_failure(ErrorCode::recipe_algorithm_unknown,
                              "algorithm must match [a-z0-9._-]{1,64}");
    }
    auto found = registry.find(algorithm);
    if (!found.has_value()) {
        return recipe_failure(ErrorCode::recipe_algorithm_unknown,
                              "algorithm is not registered: " + algorithm);
    }
    const inspection::IInspectionAlgorithm* algorithm_impl = found.value();

    const nlohmann::json& parameters = root.at("parameters");
    if (!parameters.is_object()) {
        return recipe_failure(ErrorCode::recipe_parameters_invalid,
                              "parameters must be an object");
    }
    auto validation = algorithm_impl->validate_parameters(parameters);
    if (!validation.has_value()) {
        return recipe_failure(ErrorCode::recipe_parameters_invalid, validation.failure().message);
    }

    Recipe recipe;
    recipe.schema_version = static_cast<std::uint32_t>(schema_version);
    recipe.recipe_id = std::move(recipe_id);
    recipe.algorithm = std::move(algorithm);
    recipe.parameters = parameters;
    if (auto failure = validate_capture(root.at("capture"), recipe.capture)) {
        return *failure;
    }
    if (auto failure = validate_artifacts(root.at("artifacts"), recipe.artifacts)) {
        return *failure;
    }
    return recipe;
}

}  // namespace cvforwin::recipes

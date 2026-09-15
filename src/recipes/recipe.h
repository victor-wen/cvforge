/*
 * Versioned recipe contract (recipe_store module).
 *
 * load_recipe_file() strictly parses one recipe JSON document: required and
 * unknown top-level keys are enforced, recipe_id and algorithm match their
 * frozen byte patterns, parameters must be an object accepted by the
 * referenced compiled algorithm's validate_parameters() operation, and the
 * capture/artifacts sections are bounded. Errors use the frozen recipe-store
 * numbering (1610-1619) and never escape as exceptions.
 *
 * artifacts.save_policy carries the validated text token ("always",
 * "fail_or_error", "never") required by the frozen black-box tests; the value
 * is strictly validated before a Recipe is returned.
 */

#ifndef CVFORWIN_SRC_RECIPES_RECIPE_H_
#define CVFORWIN_SRC_RECIPES_RECIPE_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "core/result.h"

namespace cvforwin::inspection {
class AlgorithmRegistry;
}  // namespace cvforwin::inspection

namespace cvforwin::recipes {

struct CaptureOverrides {
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    std::optional<double> frame_rate;
    std::optional<std::string> pixel_format;
    std::uint32_t settle_frames = 0;
};

struct ArtifactsConfig {
    std::string save_policy;
    bool required = false;
};

struct Recipe {
    std::uint32_t schema_version = 0;
    std::string recipe_id;
    std::string algorithm;
    nlohmann::json parameters;
    CaptureOverrides capture;
    ArtifactsConfig artifacts;
};

/*
 * Reads and validates one recipe file. Missing/unreadable/duplicate/unknown/
 * invalid documents fail with a config_error failure whose code is one of the
 * frozen 1610-1621, 1623 values; parameters rejected by the registered
 * algorithm are reported as recipe_parameters_invalid and keep the
 * algorithm's diagnostic message.
 */
core::Result<Recipe> load_recipe_file(const std::filesystem::path& path,
                                      const inspection::AlgorithmRegistry& registry);

}  // namespace cvforwin::recipes

#endif /* CVFORWIN_SRC_RECIPES_RECIPE_H_ */

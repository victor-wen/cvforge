#pragma once

// CVF-004 independent black-box test support (owner: test-engineer).
//
// Shared aliases, run-time temporary-directory helpers, JSON fixtures, and the
// algorithm test double used by the independent recipe-store suite. This header
// compiles only against the frozen interface headers listed in the CVF-004 test
// brief and never includes production .cpp files.
//
// Interface spellings used here are derived from the CVF-004 brief's frozen
// interface_reference; the small set of assumptions beyond that text is recorded
// in .ai/reports/CVF-004-test-red.yaml (author assumptions).
//
// Frozen interface headers first: a missing interface header must be the first
// diagnostic in the author (RED) phase.

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

#include "camera/captured_frame.h"

#include "inspection/algorithm.h"
#include "inspection/registry.h"

#include "algorithms/compiled_algorithms.h"
#include "algorithms/example_threshold.h"

#include "recipes/config.h"
#include "recipes/recipe.h"
#include "recipes/recipe_catalog.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cvf004 {

namespace core = cvforwin::core;
namespace insp = cvforwin::inspection;
namespace alg = cvforwin::algorithms;
namespace cam = cvforwin::camera;
namespace recipes = cvforwin::recipes;

using Json = nlohmann::json;

// --- run-time temporary directories ----------------------------------------

class TempDir {
public:
    explicit TempDir(std::string_view tag)
    {
        namespace fs = std::filesystem;
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        path_ = fs::temp_directory_path() / ("cvf004_" + std::string(tag) + "_" + unique);
        std::error_code error;
        fs::remove_all(path_, error);
        if (!fs::create_directories(path_, error)) {
            throw std::runtime_error("cvf004: cannot create temp directory " + path_.string());
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

// --- file helpers -----------------------------------------------------------

inline void write_text(const std::filesystem::path& file, std::string_view text)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

inline void write_json(const std::filesystem::path& file, const Json& value)
{
    write_text(file, value.dump(2));
}

inline std::string replace_once(std::string text, std::string_view from, std::string_view to)
{
    const auto position = text.find(from);
    REQUIRE(position != std::string::npos);
    text.replace(position, from.size(), to);
    return text;
}

// --- config fixtures --------------------------------------------------------

inline Json valid_config()
{
    return Json{
        {"schema_version", 1},
        {"camera",
         Json{
             {"backend", "uvc"},
             {"device_path", "/dev/cvf004-camera"},
             {"vendor_id", "1A2B"},
             {"product_id", "0C3D"},
             {"friendly_name", "CVF-004 probe camera"},
         }},
        {"base_capture",
         Json{
             {"width", 1920},
             {"height", 1080},
             {"frame_rate", 30.0},
             {"pixel_format", "bgr8"},
         }},
        {"logging",
         Json{
             {"level", "info"},
             {"max_file_bytes", 10485760},
             {"max_files", 5},
         }},
        {"retention",
         Json{
             {"max_age_days", 30},
             {"max_total_bytes", 10737418240LL},
         }},
    };
}

inline void write_config(const std::filesystem::path& config_root, const Json& config)
{
    write_json(config_root / "cvforwin.json", config);
}

// Multi-line text of a fully valid config; duplicate-key and unknown-key tests
// edit this text because nlohmann::json cannot represent duplicate keys.
inline std::string valid_config_json_text()
{
    return std::string{R"({
  "schema_version": 1,
  "camera": {
    "backend": "uvc",
    "device_path": "/dev/cvf004-camera",
    "vendor_id": "1A2B",
    "product_id": "0C3D",
    "friendly_name": "CVF-004 probe camera"
  },
  "base_capture": {
    "width": 1920,
    "height": 1080,
    "frame_rate": 30.0,
    "pixel_format": "bgr8"
  },
  "logging": {
    "level": "info",
    "max_file_bytes": 10485760,
    "max_files": 5
  },
  "retention": {
    "max_age_days": 30,
    "max_total_bytes": 10737418240
  }
})"};
}

// --- recipe fixtures --------------------------------------------------------

inline Json minimal_recipe(const std::string& recipe_id,
                           const std::string& algorithm = "example.threshold")
{
    return Json{
        {"schema_version", 1},
        {"recipe_id", recipe_id},
        {"algorithm", algorithm},
        {"parameters", Json::object()},
        {"capture", Json{{"settle_frames", 0}}},
        {"artifacts", Json{{"save_policy", "fail_or_error"}, {"required", false}}},
    };
}

inline Json valid_recipe(const std::string& recipe_id,
                         const std::string& algorithm = "example.threshold")
{
    Json recipe = minimal_recipe(recipe_id, algorithm);
    recipe["parameters"] = Json{{"threshold", 128}, {"min_pass_ratio", 0.5}};
    recipe["capture"] = Json{
        {"width", 640},
        {"height", 480},
        {"frame_rate", 15.0},
        {"pixel_format", "bgr8"},
        {"settle_frames", 3},
    };
    recipe["artifacts"] = Json{{"save_policy", "fail_or_error"}, {"required", true}};
    return recipe;
}

inline void write_recipe(const std::filesystem::path& recipes_dir, const std::string& filename,
                         const Json& recipe)
{
    write_json(recipes_dir / filename, recipe);
}

// Multi-line text of a fully valid recipe; duplicate-key and unknown-key tests
// edit this text because nlohmann::json cannot represent duplicate keys.
inline std::string valid_recipe_json_text()
{
    return std::string{R"({
  "schema_version": 1,
  "recipe_id": "probe.recipe",
  "algorithm": "example.threshold",
  "parameters": {
    "threshold": 128
  },
  "capture": {
    "width": 640,
    "height": 480,
    "frame_rate": 15.0,
    "pixel_format": "bgr8",
    "settle_frames": 3
  },
  "artifacts": {
    "save_policy": "fail_or_error",
    "required": true
  }
})"};
}

// --- registries -------------------------------------------------------------

// Builds a registry from the compiled algorithms only (example.threshold).
inline std::unique_ptr<insp::AlgorithmRegistry> make_compiled_registry()
{
    auto registry = std::make_unique<insp::AlgorithmRegistry>();
    auto registered = alg::register_compiled_algorithms(*registry);
    REQUIRE(registered.has_value());
    return registry;
}

// Deterministic IInspectionAlgorithm double whose parameter verdict is chosen by
// the test, proving that recipe validation is delegated through the registry.
class RecordingAlgorithm final : public insp::IInspectionAlgorithm {
public:
    RecordingAlgorithm(std::string key, bool reject_parameters)
        : key_(std::move(key)), reject_parameters_(reject_parameters)
    {}

    std::string_view key() const noexcept override
    {
        return key_;
    }

    core::Result<void> validate_parameters(const Json&) const override
    {
        ++validate_calls_;
        if (reject_parameters_) {
            return core::Result<void>{core::Failure{core::Status::config_error,
                                                    core::ErrorCode::algorithm_parameters_invalid,
                                                    "cvf004 stub rejects parameters"}};
        }
        return core::Result<void>{};
    }

    core::Result<insp::AlgorithmResult> inspect(const insp::AlgorithmRequest&) override
    {
        return core::Result<insp::AlgorithmResult>{
            insp::AlgorithmResult{.verdict = core::Verdict::not_evaluated,
                                  .measurements = Json::object(),
                                  .defects = Json::array(),
                                  .diagnostics = "cvf004 stub inspect is not used by recipe tests"}};
    }

    int validate_calls() const noexcept
    {
        return validate_calls_;
    }

private:
    std::string key_;
    bool reject_parameters_;
    mutable int validate_calls_ = 0;
};

struct StubRegistry {
    std::unique_ptr<insp::AlgorithmRegistry> registry;
    RecordingAlgorithm* algorithm = nullptr;
};

inline StubRegistry make_stub_registry(const std::string& key, bool reject_parameters)
{
    StubRegistry result;
    result.registry = std::make_unique<insp::AlgorithmRegistry>();
    auto algorithm = std::make_unique<RecordingAlgorithm>(key, reject_parameters);
    result.algorithm = algorithm.get();
    auto added = result.registry->add(std::move(algorithm));
    REQUIRE(added.has_value());
    return result;
}

// --- failure checks ---------------------------------------------------------

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

// --- value checks -----------------------------------------------------------

inline void check_text(std::string_view actual, std::string_view expected, const char* what)
{
    INFO(what);
    CHECK(actual == expected);
}

template <typename T>
inline void check_integer(const T& actual, std::int64_t expected, const char* what)
{
    INFO(what);
    CHECK(static_cast<std::int64_t>(actual) == expected);
}

inline void check_number(double actual, double expected, const char* what)
{
    INFO(what);
    CHECK(actual == Catch::Approx(expected));
}

// --- config/recipe failure conveniences -------------------------------------

inline void expect_config_text_failure(std::string_view text, core::Status status,
                                       core::ErrorCode code)
{
    TempDir config_root("config_text_fail");
    TempDir output_root("output_text_fail");
    write_text(config_root.path() / "cvforwin.json", text);
    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());
    check_failure(loaded, status, code);
}

inline void expect_config_failure(const Json& config, core::Status status, core::ErrorCode code)
{
    TempDir config_root("config_json_fail");
    TempDir output_root("output_json_fail");
    write_config(config_root.path(), config);
    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());
    check_failure(loaded, status, code);
}

inline void expect_recipe_text_failure(const insp::AlgorithmRegistry& registry, std::string_view text,
                                       core::Status status, core::ErrorCode code)
{
    TempDir recipes_dir("recipe_text_fail");
    write_text(recipes_dir.path() / "probe.json", text);
    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", registry);
    check_failure(loaded, status, code);
}

inline void expect_recipe_failure(const insp::AlgorithmRegistry& registry, const Json& recipe,
                                  core::Status status, core::ErrorCode code)
{
    TempDir recipes_dir("recipe_json_fail");
    write_recipe(recipes_dir.path(), "probe.json", recipe);
    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", registry);
    check_failure(loaded, status, code);
}

inline void expect_recipe_ok(const insp::AlgorithmRegistry& registry, const Json& recipe)
{
    TempDir recipes_dir("recipe_json_ok");
    write_recipe(recipes_dir.path(), "probe.json", recipe);
    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", registry);
    REQUIRE(loaded.has_value());
}

// --- catalog helpers --------------------------------------------------------

inline std::shared_ptr<const recipes::RecipeCatalog> make_catalog(const std::string& tag,
                                                                  const std::vector<std::string>& ids)
{
    TempDir recipes_dir("catalog_" + tag);
    const auto registry = make_compiled_registry();
    std::size_t index = 0;
    for (const auto& id : ids) {
        write_recipe(recipes_dir.path(), "recipe_" + std::to_string(index) + ".json",
                     minimal_recipe(id));
        ++index;
    }
    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);
    REQUIRE(loaded.has_value());
    return std::make_shared<const recipes::RecipeCatalog>(std::move(loaded.value()));
}

}  // namespace cvf004

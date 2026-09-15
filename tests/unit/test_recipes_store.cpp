// CVF-004 developer-owned recipe-store tests (not test-owner files).
//
// Focused coverage for loader edges that the independent black-box suite does
// not pin: duplicate-key detection inside sibling objects within arrays,
// selector and string-bound rules, missing sections, algorithm message
// preservation, and non-JSON-only recipe directories.

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "algorithms/compiled_algorithms.h"
#include "inspection/registry.h"
#include "recipes/config.h"
#include "recipes/recipe.h"
#include "recipes/recipe_catalog.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

using Json = nlohmann::json;
namespace core = cvforwin::core;
namespace recipes = cvforwin::recipes;
namespace insp = cvforwin::inspection;
namespace alg = cvforwin::algorithms;

class TempDir {
public:
    explicit TempDir(std::string_view tag)
    {
        namespace fs = std::filesystem;
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        path_ = fs::temp_directory_path() / ("cvf004dev_" + std::string(tag) + "_" + unique);
        std::error_code error;
        fs::remove_all(path_, error);
        if (!fs::create_directories(path_, error)) {
            throw std::runtime_error("cvf004dev: cannot create temp directory " + path_.string());
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

void write_text(const std::filesystem::path& file, std::string_view text)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

void write_json(const std::filesystem::path& file, const Json& value)
{
    write_text(file, value.dump(2));
}

Json valid_config()
{
    return Json{
        {"schema_version", 1},
        {"camera",
         Json{
             {"backend", "uvc"},
             {"device_path", "/dev/cvf004dev-camera"},
             {"vendor_id", "1A2B"},
             {"product_id", "0C3D"},
             {"friendly_name", "CVF-004 developer probe"},
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

Json recipe_document(const std::string& recipe_id, const std::string& algorithm)
{
    return Json{
        {"schema_version", 1},
        {"recipe_id", recipe_id},
        {"algorithm", algorithm},
        {"parameters", Json::object()},
        {"capture", Json{{"settle_frames", 0}}},
        {"artifacts", Json{{"save_policy", "never"}, {"required", false}}},
    };
}

std::unique_ptr<insp::AlgorithmRegistry> compiled_registry()
{
    auto registry = std::make_unique<insp::AlgorithmRegistry>();
    auto registered = alg::register_compiled_algorithms(*registry);
    REQUIRE(registered.has_value());
    return registry;
}

// Accepts any object parameters and rejects with a stable, unique message so
// the loader's message preservation can be pinned.
class DevStubAlgorithm final : public insp::IInspectionAlgorithm {
public:
    std::string_view key() const noexcept override
    {
        return "dev.stub";
    }

    core::Result<void> validate_parameters(const nlohmann::json&) const override
    {
        return core::Result<void>{
            core::Failure{core::Status::config_error, core::ErrorCode::algorithm_parameters_invalid,
                          "dev.stub preserved diagnostic"}};
    }

    core::Result<insp::AlgorithmResult> inspect(const insp::AlgorithmRequest&) override
    {
        return core::Result<insp::AlgorithmResult>{insp::AlgorithmResult{}};
    }
};

class DevAcceptAlgorithm final : public insp::IInspectionAlgorithm {
public:
    std::string_view key() const noexcept override
    {
        return "dev.accept";
    }

    core::Result<void> validate_parameters(const nlohmann::json&) const override
    {
        return core::Result<void>{};
    }

    core::Result<insp::AlgorithmResult> inspect(const insp::AlgorithmRequest&) override
    {
        return core::Result<insp::AlgorithmResult>{insp::AlgorithmResult{}};
    }
};

std::unique_ptr<insp::AlgorithmRegistry> single_algorithm_registry(
    std::unique_ptr<insp::IInspectionAlgorithm> algorithm)
{
    auto registry = std::make_unique<insp::AlgorithmRegistry>();
    auto added = registry->add(std::move(algorithm));
    REQUIRE(added.has_value());
    return registry;
}

void expect_config_failure(const Json& config, core::ErrorCode code)
{
    TempDir config_root("config");
    TempDir output_root("output");
    write_json(config_root.path() / "cvforwin.json", config);
    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
    CHECK(loaded.failure().code == code);
}

}  // namespace

TEST_CASE("CVF-004 dev: sibling objects inside array parameters are not duplicate keys",
          "[cvf-004][dev][recipes]")
{
    TempDir recipes_dir("sibling_objects");
    const auto registry = single_algorithm_registry(std::make_unique<DevAcceptAlgorithm>());
    Json samples = Json::array();
    samples.push_back(Json{{"threshold", 1}});
    samples.push_back(Json{{"threshold", 2}});
    Json recipe = recipe_document("sibling.recipe", "dev.accept");
    recipe["parameters"] = Json{{"samples", samples}};
    write_json(recipes_dir.path() / "probe.json", recipe);

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

    REQUIRE(loaded.has_value());
    CHECK(loaded.value().parameters == recipe.at("parameters"));
}

TEST_CASE("CVF-004 dev: a duplicate key inside one object of an array is rejected",
          "[cvf-004][dev][recipes][negative]")
{
    TempDir recipes_dir("sibling_duplicate");
    const auto registry = single_algorithm_registry(std::make_unique<DevAcceptAlgorithm>());
    // Duplicate keys cannot be represented by nlohmann values, so the file is
    // written as raw text.
    const std::string text = R"({
  "schema_version": 1,
  "recipe_id": "sibling.recipe",
  "algorithm": "dev.accept",
  "parameters": {
    "samples": [
      {
        "threshold": 1,
        "threshold": 2
      }
    ]
  },
  "capture": {
    "settle_frames": 0
  },
  "artifacts": {
    "save_policy": "never",
    "required": false
  }
})";
    write_text(recipes_dir.path() / "probe.json", text);

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
    CHECK(loaded.failure().code == core::ErrorCode::recipe_duplicate_key);
}

TEST_CASE("CVF-004 dev: a nested duplicate configuration key is rejected",
          "[cvf-004][dev][config][negative]")
{
    TempDir config_root("nested_duplicate");
    TempDir output_root("nested_duplicate_output");
    const std::string text = R"({
  "schema_version": 1,
  "camera": {
    "backend": "uvc",
    "device_path": "/dev/cvf004dev-camera"
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
    "max_files": 5,
    "max_files": 6
  },
  "retention": {
    "max_age_days": 30,
    "max_total_bytes": 10737418240
  }
})";
    write_text(config_root.path() / "cvforwin.json", text);

    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
    CHECK(loaded.failure().code == core::ErrorCode::config_duplicate_key);
}

TEST_CASE("CVF-004 dev: friendly_name alone never satisfies the camera selector",
          "[cvf-004][dev][config][negative]")
{
    Json config = valid_config();
    config["camera"]["device_path"] = "";
    config["camera"]["vendor_id"] = "";
    config["camera"]["product_id"] = "";
    config["camera"]["friendly_name"] = "only-a-name";

    expect_config_failure(config, core::ErrorCode::config_value_invalid);
}

TEST_CASE("CVF-004 dev: camera selector strings are bounded to 256 bytes",
          "[cvf-004][dev][config][negative]")
{
    Json config = valid_config();
    config["camera"]["device_path"] = std::string(257, 'a');

    expect_config_failure(config, core::ErrorCode::config_value_invalid);
}

TEST_CASE("CVF-004 dev: a missing configuration section is a value error",
          "[cvf-004][dev][config][negative]")
{
    SECTION("missing logging section")
    {
        Json config = valid_config();
        config.erase("logging");
        expect_config_failure(config, core::ErrorCode::config_value_invalid);
    }

    SECTION("missing schema_version")
    {
        Json config = valid_config();
        config.erase("schema_version");
        expect_config_failure(config, core::ErrorCode::config_value_invalid);
    }

    SECTION("missing base_capture section")
    {
        Json config = valid_config();
        config.erase("base_capture");
        expect_config_failure(config, core::ErrorCode::config_value_invalid);
    }
}

TEST_CASE("CVF-004 dev: a missing recipe section is a value error",
          "[cvf-004][dev][recipes][negative]")
{
    TempDir recipes_dir("missing_section");
    const auto registry = compiled_registry();
    Json recipe = recipe_document("section.recipe", "example.threshold");
    recipe.erase("artifacts");
    write_json(recipes_dir.path() / "probe.json", recipe);

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
    CHECK(loaded.failure().code == core::ErrorCode::recipe_value_invalid);
}

TEST_CASE("CVF-004 dev: a rejected parameters object preserves the algorithm diagnostic",
          "[cvf-004][dev][recipes][negative]")
{
    TempDir recipes_dir("message");
    const auto registry = single_algorithm_registry(std::make_unique<DevStubAlgorithm>());
    Json recipe = recipe_document("message.recipe", "dev.stub");
    write_json(recipes_dir.path() / "probe.json", recipe);

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
    CHECK(loaded.failure().code == core::ErrorCode::recipe_parameters_invalid);
    CHECK(loaded.failure().message == "dev.stub preserved diagnostic");
}

TEST_CASE("CVF-004 dev: a directory with no JSON recipes loads as an empty catalog",
          "[cvf-004][dev][catalog][boundary]")
{
    TempDir recipes_dir("no_json");
    const auto registry = compiled_registry();
    write_text(recipes_dir.path() / "notes.txt", "not a recipe");
    write_json(recipes_dir.path() / "backup.json.bak", recipe_document("backup", "example.threshold"));

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    REQUIRE(loaded.has_value());
    CHECK(loaded.value().size() == 0U);
    CHECK(loaded.value().recipe_ids().empty());
}

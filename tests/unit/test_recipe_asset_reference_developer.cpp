/*
 * Developer-owned CVF-106 regression tests.
 *
 * These cases are authored by the developer (not the independent test owner)
 * and cover the portable containment of recipe asset references resolved by
 * RecipeCatalog::load(). The production check must not use
 * std::filesystem::path::native() string prefixes, because native() is
 * std::wstring on Windows and a narrow ".." prefix does not compile there.
 * A first-path-component comparison is portable and cannot throw on
 * non-representable characters, so these cases pin:
 *   1. a plain reference under the assets root is accepted and prepared;
 *   2. a legitimate nested reference under the assets root is accepted;
 *   3. a ".." reference is rejected;
 *   4. a nested "a/../../x" escape is rejected;
 *   5. an absolute reference is rejected;
 *   6. a component that merely begins with dots (but is not "..") is not an
 *      escape and is accepted;
 *   7. (POSIX) a reference that canonicalizes outside the root through a
 *      symlinked directory is rejected by the catalog containment check.
 */

#include "algorithms/compiled_algorithms.h"

#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

#include "inspection/registry.h"

#include "recipes/recipe.h"
#include "recipes/recipe_catalog.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace core = cvforwin::core;
namespace insp = cvforwin::inspection;
namespace alg = cvforwin::algorithms;
namespace recipes = cvforwin::recipes;

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr std::string_view k_recipe_id = "tmpl.asset";

class TempDir {
public:
    explicit TempDir(const std::string& tag)
    {
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        path_ = fs::temp_directory_path() / ("cvf106_catalog_" + tag + "_" + unique);
        std::error_code error;
        fs::remove_all(path_, error);
        if (!fs::create_directories(path_, error)) {
            throw std::runtime_error("cannot create temp directory " + path_.string());
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const noexcept
    {
        return path_;
    }

private:
    fs::path path_;
};

// 5x4 single-channel template with non-zero variance and asymmetric structure,
// so template.match preparation succeeds and cannot mistake a mirrored match.
cv::Mat template_image()
{
    static const unsigned char k_pixels[4][5] = {
        {10, 40, 90, 160, 230},
        {200, 20, 70, 140, 210},
        {60, 130, 250, 30, 100},
        {220, 110, 15, 180, 55},
    };
    cv::Mat image(4, 5, CV_8UC1);
    for (int y = 0; y < image.rows; ++y) {
        for (int x = 0; x < image.cols; ++x) {
            image.at<unsigned char>(y, x) = k_pixels[y][x];
        }
    }
    return image;
}

void write_bytes(const fs::path& file, const std::vector<unsigned char>& bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

void write_png(const fs::path& file, const cv::Mat& image)
{
    std::vector<unsigned char> buffer;
    REQUIRE(cv::imencode(".png", image, buffer));
    write_bytes(file, buffer);
}

void write_json(const fs::path& file, const Json& value)
{
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    const std::string text = value.dump(2);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

Json asset_recipe(const std::string& reference)
{
    return Json{
        {"schema_version", 1},
        {"recipe_id", std::string(k_recipe_id)},
        {"algorithm", "template.match"},
        {"parameters",
         Json{{"template_asset", "tmpl"},
              {"roi", Json::array({0, 0, 8, 8})},
              {"method", "ccoeff_normed"},
              {"threshold", 0.8}}},
        {"capture",
         Json{{"width", 16},
              {"height", 12},
              {"frame_rate", 15.0},
              {"pixel_format", "bgr8"},
              {"settle_frames", 0}}},
        {"artifacts", Json{{"save_policy", "never"}, {"required", false}}},
        {"assets", Json{{"tmpl", reference}}},
    };
}

insp::AlgorithmRegistry make_registry()
{
    insp::AlgorithmRegistry registry;
    REQUIRE(alg::register_compiled_algorithms(registry).has_value());
    return registry;
}

core::Result<recipes::RecipeCatalog> load_catalog(const fs::path& root,
                                                  const insp::AlgorithmRegistry& registry)
{
    return recipes::RecipeCatalog::load(root / "recipes", registry);
}

}  // namespace

TEST_CASE("CVF-106 a plain relative asset reference remains accepted", "[cvf106][recipes]")
{
    TempDir root("plain");
    write_png(root.path() / "assets" / "tmpl.png", template_image());
    write_json(root.path() / "recipes" / (std::string(k_recipe_id) + ".json"),
               asset_recipe("tmpl.png"));

    insp::AlgorithmRegistry registry = make_registry();
    auto loaded = load_catalog(root.path(), registry);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().size() == 1u);
    CHECK(loaded.value().find_prepared(k_recipe_id).has_value());
}

TEST_CASE("CVF-106 a legitimate nested asset reference remains accepted", "[cvf106][recipes]")
{
    TempDir root("nested");
    write_png(root.path() / "assets" / "nested" / "tmpl.png", template_image());
    write_json(root.path() / "recipes" / (std::string(k_recipe_id) + ".json"),
               asset_recipe("nested/tmpl.png"));

    insp::AlgorithmRegistry registry = make_registry();
    auto loaded = load_catalog(root.path(), registry);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().size() == 1u);
}

TEST_CASE("CVF-106 a parent-directory asset reference is rejected", "[cvf106][recipes]")
{
    TempDir root("parent");
    write_json(root.path() / "recipes" / (std::string(k_recipe_id) + ".json"), asset_recipe(".."));

    insp::AlgorithmRegistry registry = make_registry();
    auto loaded = load_catalog(root.path(), registry);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 a nested parent-directory escape is rejected", "[cvf106][recipes]")
{
    TempDir root("nested_escape");
    write_json(root.path() / "recipes" / (std::string(k_recipe_id) + ".json"),
               asset_recipe("a/../../x"));

    insp::AlgorithmRegistry registry = make_registry();
    auto loaded = load_catalog(root.path(), registry);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 an absolute asset reference is rejected", "[cvf106][recipes]")
{
    TempDir root("absolute");
    write_json(root.path() / "recipes" / (std::string(k_recipe_id) + ".json"),
               asset_recipe("/etc/passwd"));

    insp::AlgorithmRegistry registry = make_registry();
    auto loaded = load_catalog(root.path(), registry);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
}

TEST_CASE("CVF-106 a dot-prefixed component that is not parent is not an escape",
          "[cvf106][recipes]")
{
    TempDir root("dotted");
    write_png(root.path() / "assets" / "..data" / "tmpl.png", template_image());
    write_json(root.path() / "recipes" / (std::string(k_recipe_id) + ".json"),
               asset_recipe("..data/tmpl.png"));

    insp::AlgorithmRegistry registry = make_registry();
    auto loaded = load_catalog(root.path(), registry);
    REQUIRE(loaded.has_value());
    CHECK(loaded.value().size() == 1u);
}

#ifndef _WIN32
TEST_CASE("CVF-106 a reference canonicalizing outside the assets root is rejected",
          "[cvf106][recipes]")
{
    TempDir root("symlink_escape");
    const fs::path outside = root.path() / "outside";
    fs::create_directories(outside);
    write_png(outside / "x.png", template_image());
    write_json(root.path() / "recipes" / (std::string(k_recipe_id) + ".json"),
               asset_recipe("sub/x.png"));

    fs::create_directories(root.path() / "assets");
    std::error_code error;
    fs::create_directory_symlink(outside, root.path() / "assets" / "sub", error);
    if (error) {
        SUCCEED("directory symlinks are unavailable on this host; catalog escape not exercised");
        return;
    }

    insp::AlgorithmRegistry registry = make_registry();
    auto loaded = load_catalog(root.path(), registry);
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
}
#endif

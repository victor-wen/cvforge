// CVF-004 independent black-box tests: immutable catalog load and lookup
// (brief B4, B5).
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "cvf004_test_support.h"

using namespace cvf004;

TEST_CASE("CVF-004 B4: catalog loads every *.json file directly under recipes_dir with ids "
          "sorted lexicographically",
          "[cvf-004][B4][catalog]")
{
    TempDir recipes_dir("catalog_sorted");
    const auto registry = make_compiled_registry();
    // Deliberately shuffled file order; ids exercise ASCII byte ordering
    // (digits < uppercase < lowercase, '-' < '.').
    const std::vector<std::string> ids = {"b", "A", "a", "a.b", "a-b", "Z9", "9", "10"};
    std::size_t index = 0;
    for (const auto& id : ids) {
        write_recipe(recipes_dir.path(), "file_" + std::to_string(index) + ".json",
                     minimal_recipe(id));
        ++index;
    }

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    REQUIRE(loaded.has_value());
    const recipes::RecipeCatalog& catalog = loaded.value();
    check_integer(catalog.size(), 8, "size");
    const std::vector<std::string> expected = {"10", "9", "A", "Z9", "a", "a-b", "a.b", "b"};
    CHECK(catalog.recipe_ids() == expected);
}

TEST_CASE("CVF-004 B4: an empty recipes directory yields a valid size-0 catalog",
          "[cvf-004][B4][catalog][boundary]")
{
    TempDir recipes_dir("catalog_empty");
    const auto registry = make_compiled_registry();

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    REQUIRE(loaded.has_value());
    CHECK(loaded.value().size() == 0u);
    CHECK(loaded.value().recipe_ids().empty());
}

TEST_CASE("CVF-004 B4: a single-recipe directory yields a size-1 catalog",
          "[cvf-004][B4][catalog][boundary]")
{
    TempDir recipes_dir("catalog_single");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "only.json", valid_recipe("only.recipe"));

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    REQUIRE(loaded.has_value());
    check_integer(loaded.value().size(), 1, "size");
    CHECK(loaded.value().recipe_ids() == std::vector<std::string>{"only.recipe"});
}

TEST_CASE("CVF-004 B4 negative: a missing recipes directory reports recipes_dir_missing",
          "[cvf-004][B4][catalog][negative]")
{
    TempDir recipes_dir("catalog_missing_dir");
    const auto registry = make_compiled_registry();

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path() / "absent", *registry);

    check_failure(loaded, core::Status::config_error, core::ErrorCode::recipes_dir_missing);
}

TEST_CASE("CVF-004 B4: only direct *.json files are loaded; nested files and other extensions "
          "are ignored",
          "[cvf-004][B4][catalog]")
{
    TempDir recipes_dir("catalog_filter");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "direct.json", minimal_recipe("direct.recipe"));

    std::filesystem::create_directories(recipes_dir.path() / "nested");
    write_recipe(recipes_dir.path() / "nested", "nested.json", minimal_recipe("nested.recipe"));
    write_recipe(recipes_dir.path(), "notes.txt", minimal_recipe("text.recipe"));
    write_recipe(recipes_dir.path(), "backup.json.bak", minimal_recipe("backup.recipe"));

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    REQUIRE(loaded.has_value());
    check_integer(loaded.value().size(), 1, "size");
    CHECK(loaded.value().recipe_ids() == std::vector<std::string>{"direct.recipe"});
}

TEST_CASE("CVF-004 B4 negative: one invalid recipe fails the whole load with no catalog",
          "[cvf-004][B4][catalog][negative]")
{
    TempDir recipes_dir("catalog_invalid");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "good_a.json", minimal_recipe("good.a"));
    write_recipe(recipes_dir.path(), "good_b.json", minimal_recipe("good.b"));
    write_text(recipes_dir.path() / "broken.json", R"({"schema_version": 1, "recipe_id":)");

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    check_failure(loaded, core::Status::config_error, core::ErrorCode::recipe_parse_error);
}

TEST_CASE("CVF-004 B4 negative: duplicate recipe_id across files reports recipe_id_duplicate "
          "with no catalog",
          "[cvf-004][B4][catalog][negative]")
{
    TempDir recipes_dir("catalog_duplicate");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "first.json", valid_recipe("duplicate.recipe"));
    write_recipe(recipes_dir.path(), "second.json", valid_recipe("duplicate.recipe"));

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    check_failure(loaded, core::Status::config_error, core::ErrorCode::recipe_id_duplicate);
}

#ifndef _WIN32
TEST_CASE("CVF-004 B4 negative: an unreadable recipe file fails the catalog load with "
          "recipe_io_error",
          "[cvf-004][B4][catalog][negative]")
{
    if (::geteuid() == 0) {
        SKIP("permission bits are not enforced for the superuser");
    }

    TempDir recipes_dir("catalog_unreadable");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "good.json", minimal_recipe("good.recipe"));
    const auto unreadable = recipes_dir.path() / "unreadable.json";
    write_recipe(recipes_dir.path(), "unreadable.json", minimal_recipe("unreadable.recipe"));
    std::filesystem::permissions(unreadable, std::filesystem::perms::none);

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    check_failure(loaded, core::Status::config_error, core::ErrorCode::recipe_io_error);

    std::filesystem::permissions(unreadable, std::filesystem::perms::owner_all);
}
#endif

TEST_CASE("CVF-004 B5: find returns the loaded recipe fields for a known id",
          "[cvf-004][B5][catalog]")
{
    TempDir recipes_dir("catalog_find");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "alpha.json", minimal_recipe("alpha"));
    Json beta = valid_recipe("beta");
    beta["parameters"] = Json{{"threshold", 200}};
    write_recipe(recipes_dir.path(), "beta.json", beta);
    write_recipe(recipes_dir.path(), "gamma.json", minimal_recipe("gamma"));

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);
    REQUIRE(loaded.has_value());
    const recipes::RecipeCatalog& catalog = loaded.value();

    auto found = catalog.find("beta");
    REQUIRE(found.has_value());
    REQUIRE(found.value() != nullptr);
    const recipes::Recipe* recipe = found.value();
    check_text(recipe->recipe_id, "beta", "recipe_id");
    check_text(recipe->algorithm, "example.threshold", "algorithm");
    CHECK(recipe->parameters == Json{{"threshold", 200}});
    check_integer(recipe->capture.settle_frames, 3, "capture.settle_frames");
    check_text(recipe->artifacts.save_policy, "fail_or_error", "artifacts.save_policy");
    CHECK(recipe->artifacts.required == true);
}

TEST_CASE("CVF-004 B5 negative: find(unknown) reports recipe_not_found",
          "[cvf-004][B5][catalog][negative]")
{
    TempDir recipes_dir("catalog_not_found");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "alpha.json", minimal_recipe("alpha"));

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);
    REQUIRE(loaded.has_value());
    const recipes::RecipeCatalog& catalog = loaded.value();

    auto found = catalog.find("missing.recipe");

    REQUIRE_FALSE(found.has_value());
    CHECK(found.failure().status == core::Status::recipe_not_found);
    CHECK(found.failure().code == core::ErrorCode::recipe_not_found);
}

TEST_CASE("CVF-004 B5: recipe_ids and size reflect the loaded set",
          "[cvf-004][B5][catalog]")
{
    TempDir recipes_dir("catalog_ids");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "one.json", minimal_recipe("second.recipe"));
    write_recipe(recipes_dir.path(), "two.json", minimal_recipe("first.recipe"));
    write_recipe(recipes_dir.path(), "three.json", minimal_recipe("third.recipe"));

    auto loaded = recipes::RecipeCatalog::load(recipes_dir.path(), *registry);

    REQUIRE(loaded.has_value());
    const recipes::RecipeCatalog& catalog = loaded.value();
    const std::vector<std::string> ids = catalog.recipe_ids();
    CHECK(ids == std::vector<std::string>{"first.recipe", "second.recipe", "third.recipe"});
    check_integer(catalog.size(), 3, "size");
    CHECK(catalog.size() == ids.size());
    for (const auto& id : ids) {
        INFO("id: " << id);
        auto found = catalog.find(id);
        REQUIRE(found.has_value());
        REQUIRE(found.value() != nullptr);
        check_text(found.value()->recipe_id, id, "recipe_id");
    }
}

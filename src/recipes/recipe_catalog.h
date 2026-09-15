/*
 * Immutable recipe catalog and atomic holder (recipe_store module).
 *
 * RecipeCatalog::load() reads every *.json file directly under recipes_dir
 * (non-recursive, sorted by filename) and returns a catalog only when every
 * recipe is valid and every recipe_id is unique. The catalog is immutable
 * after load: find() returns a pointer into the owned recipe vector, and
 * recipe_ids() is sorted lexicographically.
 *
 * RecipeHolder publishes a complete catalog with a single atomic store, so a
 * concurrent current() call always observes either the previous or the new
 * complete catalog, never a partial set.
 */

#ifndef CVFORWIN_SRC_RECIPES_RECIPE_CATALOG_H_
#define CVFORWIN_SRC_RECIPES_RECIPE_CATALOG_H_

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "recipes/recipe.h"

namespace cvforwin::inspection {
class AlgorithmRegistry;
}  // namespace cvforwin::inspection

namespace cvforwin::recipes {

class RecipeCatalog {
public:
    RecipeCatalog(RecipeCatalog&&) noexcept = default;
    RecipeCatalog& operator=(RecipeCatalog&&) noexcept = default;
    RecipeCatalog(const RecipeCatalog&) = delete;
    RecipeCatalog& operator=(const RecipeCatalog&) = delete;
    ~RecipeCatalog() = default;

    /*
     * Loads all recipes under recipes_dir. A missing directory fails with
     * config_error/recipes_dir_missing; an empty directory yields a valid
     * size-0 catalog. Any invalid recipe or duplicate recipe_id fails the
     * whole load and produces no catalog.
     */
    static core::Result<RecipeCatalog> load(const std::filesystem::path& recipes_dir,
                                            const inspection::AlgorithmRegistry& registry);

    /* Stable pointer into the catalog, or recipe_not_found/1622. */
    core::Result<const Recipe*> find(std::string_view recipe_id) const;

    /* Recipe ids in lexicographic byte order. */
    std::vector<std::string> recipe_ids() const;

    std::size_t size() const noexcept;

private:
    RecipeCatalog(std::vector<Recipe> recipes, std::map<std::string, std::size_t> index);

    std::vector<Recipe> recipes_;
    std::map<std::string, std::size_t> index_;
};

class RecipeHolder {
public:
    /* Atomically replaces the current catalog; never throws. */
    void publish(std::shared_ptr<const RecipeCatalog> catalog) noexcept;

    /* Complete catalog snapshot; nullptr until the first publish. */
    std::shared_ptr<const RecipeCatalog> current() const noexcept;

private:
    std::atomic<std::shared_ptr<const RecipeCatalog>> catalog_{nullptr};
};

}  // namespace cvforwin::recipes

#endif /* CVFORWIN_SRC_RECIPES_RECIPE_CATALOG_H_ */

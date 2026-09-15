#include "recipes/recipe_catalog.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "inspection/registry.h"

namespace cvforwin::recipes {

namespace {

using core::ErrorCode;
using core::Status;

constexpr std::string_view k_recipe_extension = ".json";

core::Failure catalog_failure(ErrorCode code, std::string message)
{
    return core::make_failure(Status::config_error, code, std::move(message));
}

}  // namespace

RecipeCatalog::RecipeCatalog(std::vector<Recipe> recipes, std::map<std::string, std::size_t> index)
    : recipes_(std::move(recipes)),
      index_(std::move(index))
{
}

core::Result<RecipeCatalog> RecipeCatalog::load(const std::filesystem::path& recipes_dir,
                                                const inspection::AlgorithmRegistry& registry)
{
    std::error_code error;
    const bool directory = std::filesystem::is_directory(recipes_dir, error);
    if (error || !directory) {
        return catalog_failure(ErrorCode::recipes_dir_missing, "recipes directory is missing");
    }

    std::vector<std::filesystem::path> files;
    std::filesystem::directory_iterator iterator(recipes_dir, error);
    if (error) {
        return catalog_failure(ErrorCode::config_io_error, "recipes directory cannot be read");
    }
    const std::filesystem::directory_iterator finish;
    while (iterator != finish) {
        const std::filesystem::directory_entry& entry = *iterator;
        std::error_code entry_error;
        if (entry.is_regular_file(entry_error) &&
            entry.path().extension() == k_recipe_extension) {
            files.push_back(entry.path());
        }
        iterator.increment(error);
        if (error) {
            return catalog_failure(ErrorCode::config_io_error, "recipes directory cannot be read");
        }
    }
    std::sort(files.begin(), files.end(), [](const std::filesystem::path& left, const std::filesystem::path& right) {
        return left.filename().string() < right.filename().string();
    });

    std::vector<Recipe> recipes;
    std::map<std::string, std::size_t> index;
    recipes.reserve(files.size());
    for (const std::filesystem::path& file : files) {
        auto loaded = load_recipe_file(file, registry);
        if (!loaded.has_value()) {
            return loaded.failure();
        }
        const std::string recipe_id = loaded.value().recipe_id;
        if (index.find(recipe_id) != index.end()) {
            return catalog_failure(ErrorCode::recipe_id_duplicate,
                                   "duplicate recipe_id: " + recipe_id);
        }
        index.emplace(recipe_id, recipes.size());
        recipes.push_back(std::move(loaded.value()));
    }
    return RecipeCatalog{std::move(recipes), std::move(index)};
}

core::Result<const Recipe*> RecipeCatalog::find(std::string_view recipe_id) const
{
    const auto entry = index_.find(std::string{recipe_id});
    if (entry == index_.end()) {
        return core::make_failure(Status::recipe_not_found, ErrorCode::recipe_not_found,
                                  "recipe is not present in the catalog: " +
                                      std::string{recipe_id});
    }
    return &recipes_.at(entry->second);
}

std::vector<std::string> RecipeCatalog::recipe_ids() const
{
    std::vector<std::string> ids;
    ids.reserve(index_.size());
    for (const auto& entry : index_) {
        ids.push_back(entry.first);
    }
    return ids;
}

std::size_t RecipeCatalog::size() const noexcept
{
    return recipes_.size();
}

void RecipeHolder::publish(std::shared_ptr<const RecipeCatalog> catalog) noexcept
{
    catalog_.store(std::move(catalog), std::memory_order_release);
}

std::shared_ptr<const RecipeCatalog> RecipeHolder::current() const noexcept
{
    return catalog_.load(std::memory_order_acquire);
}

}  // namespace cvforwin::recipes

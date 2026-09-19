#include "recipes/recipe_catalog.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "inspection/registry.h"

namespace cvforwin::recipes {

namespace {

using core::ErrorCode;
using core::Status;

constexpr std::string_view k_recipe_extension = ".json";
constexpr std::size_t k_max_asset_bytes = 16777216u;
constexpr std::size_t k_max_total_asset_bytes = 67108864u;

core::Failure catalog_failure(ErrorCode code, std::string message)
{
    return core::make_failure(Status::config_error, code, std::move(message));
}

std::optional<core::Failure> asset_failure(std::string message)
{
    return catalog_failure(ErrorCode::recipe_io_error, std::move(message));
}

/*
 * Resolves one validated relative reference beneath assets_root and reads its
 * bounded bytes. Rejects missing/unreadable files, non-regular files, links or
 * reparse escapes (the canonical path must stay beneath the canonical assets
 * root), and encoded content over the per-file or running total bound.
 */
std::optional<core::Failure> load_asset_bytes(const std::filesystem::path& assets_root,
                                              const RecipeAsset& asset,
                                              std::uint64_t& running_total,
                                              std::vector<std::uint8_t>& out)
{
    const std::filesystem::path candidate = assets_root / asset.reference;

    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(candidate, error);
    if (error || !std::filesystem::exists(status)) {
        return asset_failure("recipe asset is missing: " + asset.reference);
    }
    if (std::filesystem::is_symlink(status)) {
        return asset_failure("recipe asset must be a regular non-link file: " + asset.reference);
    }
    if (!std::filesystem::is_regular_file(status)) {
        return asset_failure("recipe asset must be a regular file: " + asset.reference);
    }

    const std::filesystem::path root_canonical = std::filesystem::weakly_canonical(assets_root, error);
    if (error) {
        return asset_failure("recipe assets root cannot be resolved");
    }
    const std::filesystem::path file_canonical = std::filesystem::weakly_canonical(candidate, error);
    if (error) {
        return asset_failure("recipe asset cannot be resolved: " + asset.reference);
    }
    const std::filesystem::path relative = file_canonical.lexically_relative(root_canonical);
    if (relative.empty() || relative.native().starts_with("..")) {
        return asset_failure("recipe asset escapes the assets root: " + asset.reference);
    }

    const std::uintmax_t size = std::filesystem::file_size(candidate, error);
    if (error) {
        return asset_failure("recipe asset size cannot be read: " + asset.reference);
    }
    if (size > k_max_asset_bytes) {
        return asset_failure("recipe asset exceeds the " + std::to_string(k_max_asset_bytes) +
                             "-byte bound: " + asset.reference);
    }
    if (running_total + static_cast<std::uint64_t>(size) > k_max_total_asset_bytes) {
        return asset_failure("recipe assets exceed the " + std::to_string(k_max_total_asset_bytes) +
                             "-byte total bound");
    }

    std::ifstream stream(candidate, std::ios::binary);
    if (!stream) {
        return asset_failure("recipe asset cannot be opened: " + asset.reference);
    }
    out.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    if (stream.bad() || out.size() != static_cast<std::size_t>(size)) {
        return asset_failure("recipe asset cannot be read: " + asset.reference);
    }
    running_total += static_cast<std::uint64_t>(out.size());
    return std::nullopt;
}

}  // namespace

RecipeCatalog::RecipeCatalog(std::vector<Recipe> recipes, std::map<std::string, std::size_t> index,
                             std::vector<std::unique_ptr<inspection::IPreparedAlgorithm>> prepared)
    : recipes_(std::move(recipes)),
      index_(std::move(index)),
      prepared_(std::move(prepared))
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
    std::vector<std::unique_ptr<inspection::IPreparedAlgorithm>> prepared;
    recipes.reserve(files.size());
    prepared.reserve(files.size());
    const std::filesystem::path assets_root = recipes_dir.parent_path() / "assets";

    for (const std::filesystem::path& file : files) {
        auto loaded = load_recipe_file(file, registry);
        if (!loaded.has_value()) {
            return loaded.failure();
        }
        Recipe recipe = std::move(loaded).value();
        const std::string recipe_id = recipe.recipe_id;
        if (index.find(recipe_id) != index.end()) {
            return catalog_failure(ErrorCode::recipe_id_duplicate,
                                   "duplicate recipe_id: " + recipe_id);
        }

        const core::Result<const inspection::IInspectionAlgorithm*> algorithm =
            registry.find(recipe.algorithm);
        if (!algorithm.has_value()) {
            return algorithm.failure();
        }

        inspection::AlgorithmAssetBundle bundle;
        std::uint64_t running_total = 0;
        for (const RecipeAsset& asset : recipe.assets) {
            std::vector<std::uint8_t> bytes;
            if (auto failure = load_asset_bytes(assets_root, asset, running_total, bytes)) {
                return *failure;
            }
            inspection::AlgorithmAsset bundle_asset;
            bundle_asset.key = asset.key;
            bundle_asset.reference = asset.reference;
            bundle_asset.bytes = std::move(bytes);
            bundle.assets.push_back(std::move(bundle_asset));
        }

        auto prepared_result = algorithm.value()->prepare(recipe.parameters, bundle);
        if (!prepared_result.has_value()) {
            return prepared_result.failure();
        }

        index.emplace(recipe_id, recipes.size());
        recipes.push_back(std::move(recipe));
        prepared.push_back(std::move(prepared_result).value());
    }
    return RecipeCatalog{std::move(recipes), std::move(index), std::move(prepared)};
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

core::Result<const inspection::IPreparedAlgorithm*> RecipeCatalog::find_prepared(
    std::string_view recipe_id) const
{
    const auto entry = index_.find(std::string{recipe_id});
    if (entry == index_.end()) {
        return core::make_failure(Status::recipe_not_found, ErrorCode::recipe_not_found,
                                  "recipe is not present in the catalog: " +
                                      std::string{recipe_id});
    }
    const std::size_t position = entry->second;
    if (position >= prepared_.size() || !prepared_.at(position)) {
        return core::make_failure(Status::internal_error, ErrorCode::internal_unexpected,
                                  "recipe has no prepared algorithm state: " + std::string{recipe_id});
    }
    return prepared_.at(position).get();
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

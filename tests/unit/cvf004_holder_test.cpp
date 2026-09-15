// CVF-004 independent black-box tests: atomic recipe holder and all-or-nothing
// publication (brief B6, B7).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cvf004_test_support.h"

using namespace cvf004;

TEST_CASE("CVF-004 B6: publish is noexcept and current defaults to nullptr",
          "[cvf-004][B6][holder]")
{
    static_assert(noexcept(std::declval<recipes::RecipeHolder&>().publish(
                      std::declval<std::shared_ptr<const recipes::RecipeCatalog>>())),
                  "publish must be noexcept per the frozen interface");

    recipes::RecipeHolder holder;

    CHECK(holder.current() == nullptr);
}

TEST_CASE("CVF-004 B6: publish replaces the current catalog atomically",
          "[cvf-004][B6][holder]")
{
    const auto catalog_a = make_catalog("holder_a", {"alpha"});
    const auto catalog_b = make_catalog("holder_b", {"beta", "gamma"});
    recipes::RecipeHolder holder;

    holder.publish(catalog_a);
    CHECK(holder.current() == catalog_a);
    check_integer(holder.current()->size(), 1, "size after publish A");

    holder.publish(catalog_b);
    CHECK(holder.current() == catalog_b);
    CHECK(holder.current() != catalog_a);
    check_integer(holder.current()->size(), 2, "size after publish B");

    holder.publish(catalog_a);
    CHECK(holder.current() == catalog_a);
    check_integer(holder.current()->size(), 1, "size after republish A");
}

TEST_CASE("CVF-004 B6: concurrent readers never observe a partial or mixed catalog",
          "[cvf-004][B6][holder]")
{
    const auto catalog_a = make_catalog("conc_a", {"alpha"});
    const auto catalog_b = make_catalog("conc_b", {"beta", "gamma"});
    const auto catalog_c = make_catalog("conc_c", {"delta", "epsilon", "zeta"});
    const std::vector<std::vector<std::string>> expected_id_sets = {
        {"alpha"},
        {"beta", "gamma"},
        {"delta", "epsilon", "zeta"},
    };

    recipes::RecipeHolder holder;
    holder.publish(catalog_a);

    constexpr int kReaderCount = 3;
    constexpr int kPublishRounds = 200;

    std::atomic<bool> stop{false};
    std::atomic<int> violations{0};
    std::atomic<int> observations{0};
    std::atomic<int> readers_ready{0};
    // Set by the writer once the scenario is deterministically concurrent: every
    // reader thread has been scheduled and observations are already flowing
    // before the first publish. This removes the scheduler race that let the
    // writer finish before readers were first scheduled (seen once under ASan)
    // without weakening the partial/mixed-catalog invariant assertions.
    std::atomic<bool> concurrency_established{false};

    const auto observe = [&]() {
        const std::shared_ptr<const recipes::RecipeCatalog> snapshot = holder.current();
        if (snapshot == nullptr) {
            violations.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::vector<std::string> ids = snapshot->recipe_ids();
        bool known = false;
        for (const auto& expected : expected_id_sets) {
            if (ids == expected) {
                known = true;
                break;
            }
        }
        if (!known || ids.size() != snapshot->size()) {
            violations.fetch_add(1, std::memory_order_relaxed);
        }
        observations.fetch_add(1);
    };

    std::vector<std::thread> readers;
    readers.reserve(kReaderCount);

    for (int index = 0; index < kReaderCount; ++index) {
        readers.emplace_back([&]() {
            readers_ready.fetch_add(1);
            while (!stop.load()) {
                observe();
            }
        });
    }

    std::thread writer([&]() {
        // Bounded wait with a generous timeout: the writer never publishes until
        // the readers are live, and a scheduling failure is reported
        // deterministically through concurrency_established instead of
        // degrading into a writer-only run or hanging.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline) {
            if (readers_ready.load() == kReaderCount && observations.load() >= kReaderCount) {
                concurrency_established.store(true);
                break;
            }
            std::this_thread::yield();
        }

        for (int round = 0; round < kPublishRounds; ++round) {
            holder.publish(catalog_a);
            holder.publish(catalog_b);
            holder.publish(catalog_c);
        }
        stop.store(true);
    });

    writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    CHECK(concurrency_established.load());
    CHECK(violations.load() == 0);
    CHECK(observations.load() > 0);
}

TEST_CASE("CVF-004 B7: a failed candidate load leaves the published catalog unchanged",
          "[cvf-004][B7][holder][negative]")
{
    const auto catalog_a = make_catalog("hold_a", {"alpha", "beta"});
    const auto registry = make_compiled_registry();
    recipes::RecipeHolder holder;
    holder.publish(catalog_a);

    SECTION("candidate directory contains a malformed recipe")
    {
        TempDir candidate("hold_bad_recipe");
        write_recipe(candidate.path(), "good.json", minimal_recipe("gamma"));
        write_text(candidate.path() / "broken.json", R"({"schema_version": 1,)");

        auto loaded = recipes::RecipeCatalog::load(candidate.path(), *registry);

        check_failure(loaded, core::Status::config_error, core::ErrorCode::recipe_parse_error);
        CHECK(holder.current() == catalog_a);
        check_integer(holder.current()->size(), 2, "size after failed load");
    }

    SECTION("candidate directory contains a duplicate recipe_id")
    {
        TempDir candidate("hold_dup_recipe");
        write_recipe(candidate.path(), "first.json", minimal_recipe("gamma"));
        write_recipe(candidate.path(), "second.json", minimal_recipe("gamma"));

        auto loaded = recipes::RecipeCatalog::load(candidate.path(), *registry);

        check_failure(loaded, core::Status::config_error, core::ErrorCode::recipe_id_duplicate);
        CHECK(holder.current() == catalog_a);
    }

    SECTION("candidate directory is missing")
    {
        TempDir parent("hold_missing_parent");
        auto loaded = recipes::RecipeCatalog::load(parent.path() / "absent", *registry);

        check_failure(loaded, core::Status::config_error, core::ErrorCode::recipes_dir_missing);
        CHECK(holder.current() == catalog_a);
    }
}

TEST_CASE("CVF-004 B7: a fully valid candidate catalog replaces the holder contents",
          "[cvf-004][B7][holder]")
{
    const auto catalog_a = make_catalog("replace_a", {"alpha"});
    const auto catalog_b = make_catalog("replace_b", {"beta", "gamma"});
    recipes::RecipeHolder holder;
    holder.publish(catalog_a);

    holder.publish(catalog_b);

    CHECK(holder.current() == catalog_b);
    CHECK(holder.current()->find("alpha").has_value() == false);
    REQUIRE(holder.current()->find("beta").has_value());
    CHECK(holder.current()->find("beta").value() != nullptr);
    CHECK(holder.current()->find("beta").value()->recipe_id == "beta");
}

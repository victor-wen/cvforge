// CVF-003 independent black-box tests: AlgorithmRegistry semantics (brief B1, B2).
#include <memory>
#include <string>
#include <vector>

#include "cvf003_test_support.h"

using namespace cvf003;

TEST_CASE("CVF-003 B1: a fresh registry is empty and keys() is empty", "[cvf-003][B1][registry]")
{
    insp::AlgorithmRegistry registry;

    CHECK(registry.empty());
    CHECK(registry.size() == 0u);
    CHECK(registry.keys().empty());
}

TEST_CASE("CVF-003 B1: a valid add succeeds and find returns the exact added instance",
          "[cvf-003][B1][registry]")
{
    insp::AlgorithmRegistry registry;
    auto algorithm = std::make_unique<StubAlgorithm>("probe.key");
    const auto* raw = algorithm.get();

    auto added = registry.add(std::move(algorithm));

    REQUIRE(added.has_value());
    CHECK_FALSE(registry.empty());
    CHECK(registry.size() == 1u);
    CHECK(registry.keys() == std::vector<std::string>{"probe.key"});

    auto found = registry.find("probe.key");
    REQUIRE(found.has_value());
    CHECK(found.value() == raw);
}

TEST_CASE("CVF-003 B1: keys() preserves registration order", "[cvf-003][B1][registry]")
{
    insp::AlgorithmRegistry registry;

    REQUIRE(registry.add(std::make_unique<StubAlgorithm>("z.key")).has_value());
    REQUIRE(registry.add(std::make_unique<StubAlgorithm>("a.key")).has_value());
    REQUIRE(registry.add(std::make_unique<StubAlgorithm>("m.key")).has_value());

    CHECK(registry.keys() == std::vector<std::string>{"z.key", "a.key", "m.key"});
    CHECK(registry.size() == 3u);
}

TEST_CASE("CVF-003 B1 negative: add(nullptr) is rejected as an invalid key",
          "[cvf-003][B1][registry][negative]")
{
    insp::AlgorithmRegistry registry;

    auto added = registry.add(std::unique_ptr<insp::IInspectionAlgorithm>{});

    REQUIRE_FALSE(added.has_value());
    check_failure(added.failure(), core::Status::invalid_argument,
                  core::ErrorCode::algorithm_key_invalid);
    CHECK(registry.empty());
    CHECK(registry.size() == 0u);
}

TEST_CASE("CVF-003 B1 negative: empty, uppercase, space, and over-long keys are rejected",
          "[cvf-003][B1][registry][negative]")
{
    insp::AlgorithmRegistry registry;
    const std::vector<std::string> invalid_keys = {
        "",
        "Bad.Key",
        "bad key",
        std::string(65, 'a'),
    };

    for (const auto& key : invalid_keys) {
        INFO("key: [" << key << "]");
        auto added = registry.add(std::make_unique<StubAlgorithm>(key));
        REQUIRE_FALSE(added.has_value());
        check_failure(added.failure(), core::Status::invalid_argument,
                      core::ErrorCode::algorithm_key_invalid);
    }
    CHECK(registry.empty());
}

TEST_CASE("CVF-003 B1 boundary: a 64-character key is accepted and a 65-character key is rejected",
          "[cvf-003][B1][registry][boundary]")
{
    insp::AlgorithmRegistry registry;
    const std::string key_64(64, 'a');

    REQUIRE(registry.add(std::make_unique<StubAlgorithm>(key_64)).has_value());
    CHECK(registry.size() == 1u);
    auto found = registry.find(key_64);
    REQUIRE(found.has_value());

    auto rejected = registry.add(std::make_unique<StubAlgorithm>(std::string(65, 'a')));
    REQUIRE_FALSE(rejected.has_value());
    check_failure(rejected.failure(), core::Status::invalid_argument,
                  core::ErrorCode::algorithm_key_invalid);
    CHECK(registry.size() == 1u);
}

TEST_CASE("CVF-003 B1 boundary: the documented key alphabet [a-z0-9._-] is accepted",
          "[cvf-003][B1][registry][boundary]")
{
    insp::AlgorithmRegistry registry;
    const std::string key = "a.b-c_d0e9";

    REQUIRE(registry.add(std::make_unique<StubAlgorithm>(key)).has_value());
    auto found = registry.find(key);
    REQUIRE(found.has_value());
    CHECK(registry.size() == 1u);
}

TEST_CASE("CVF-003 B1 negative: a duplicate key is rejected and the original entry is retained",
          "[cvf-003][B1][registry][negative]")
{
    insp::AlgorithmRegistry registry;
    auto original = std::make_unique<StubAlgorithm>("dup.key");
    const auto* raw = original.get();
    REQUIRE(registry.add(std::move(original)).has_value());

    auto duplicate = registry.add(std::make_unique<StubAlgorithm>("dup.key"));

    REQUIRE_FALSE(duplicate.has_value());
    check_failure(duplicate.failure(), core::Status::algorithm_error,
                  core::ErrorCode::algorithm_duplicate_key);
    CHECK(registry.size() == 1u);
    CHECK(registry.keys() == std::vector<std::string>{"dup.key"});
    auto found = registry.find("dup.key");
    REQUIRE(found.has_value());
    CHECK(found.value() == raw);
}

TEST_CASE("CVF-003 B1 negative: find(unknown) fails with algorithm_not_found",
          "[cvf-003][B1][registry][negative]")
{
    insp::AlgorithmRegistry registry;
    REQUIRE(registry.add(std::make_unique<StubAlgorithm>("known.key")).has_value());

    auto found = registry.find("unknown.key");
    REQUIRE_FALSE(found.has_value());
    check_failure(found.failure(), core::Status::algorithm_error,
                  core::ErrorCode::algorithm_not_found);

    insp::AlgorithmRegistry empty_registry;
    auto missing = empty_registry.find("anything.key");
    REQUIRE_FALSE(missing.has_value());
    check_failure(missing.failure(), core::Status::algorithm_error,
                  core::ErrorCode::algorithm_not_found);
}

TEST_CASE("CVF-003 B2: register_compiled_algorithms publishes example.threshold on a fresh registry",
          "[cvf-003][B2][registry]")
{
    insp::AlgorithmRegistry registry;

    auto registered = alg::register_compiled_algorithms(registry);

    REQUIRE(registered.has_value());
    CHECK(registry.size() == 1u);
    auto found = registry.find("example.threshold");
    REQUIRE(found.has_value());
    REQUIRE(found.value() != nullptr);
    CHECK(found.value()->key() == "example.threshold");
}

TEST_CASE("CVF-003 B2 negative: a second registration fails with algorithm_duplicate_key and keeps "
          "the original",
          "[cvf-003][B2][registry][negative]")
{
    insp::AlgorithmRegistry registry;
    REQUIRE(alg::register_compiled_algorithms(registry).has_value());
    const auto* original = registry.find("example.threshold").value();
    REQUIRE(original != nullptr);

    auto again = alg::register_compiled_algorithms(registry);

    REQUIRE_FALSE(again.has_value());
    check_failure(again.failure(), core::Status::algorithm_error,
                  core::ErrorCode::algorithm_duplicate_key);
    CHECK(registry.size() == 1u);
    CHECK(registry.find("example.threshold").value() == original);
}

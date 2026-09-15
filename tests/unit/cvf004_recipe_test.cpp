// CVF-004 independent black-box tests: strict recipes and algorithm-aware
// parameter validation (brief B1, B3).
#include <cstddef>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "cvf004_test_support.h"

using namespace cvf004;

TEST_CASE("CVF-004 B1: a valid recipe file loads with every documented field preserved",
          "[cvf-004][B1][recipe]")
{
    TempDir recipes_dir("recipe_ok");
    const auto registry = make_compiled_registry();
    const Json recipe = valid_recipe("probe.recipe");
    write_recipe(recipes_dir.path(), "probe.json", recipe);

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

    REQUIRE(loaded.has_value());
    const recipes::Recipe& value = loaded.value();
    check_integer(value.schema_version, 1, "schema_version");
    check_text(value.recipe_id, "probe.recipe", "recipe_id");
    check_text(value.algorithm, "example.threshold", "algorithm");
    CHECK(value.parameters == recipe.at("parameters"));
    REQUIRE(value.capture.width.has_value());
    REQUIRE(value.capture.height.has_value());
    REQUIRE(value.capture.frame_rate.has_value());
    REQUIRE(value.capture.pixel_format.has_value());
    check_integer(*value.capture.width, 640, "capture.width");
    check_integer(*value.capture.height, 480, "capture.height");
    check_number(*value.capture.frame_rate, 15.0, "capture.frame_rate");
    check_text(*value.capture.pixel_format, "bgr8", "capture.pixel_format");
    check_integer(value.capture.settle_frames, 3, "capture.settle_frames");
    check_text(value.artifacts.save_policy, "fail_or_error", "artifacts.save_policy");
    CHECK(value.artifacts.required == true);
}

TEST_CASE("CVF-004 B1: capture overrides are optional and defaults for example.threshold load",
          "[cvf-004][B1][recipe]")
{
    TempDir recipes_dir("recipe_minimal");
    const auto registry = make_compiled_registry();
    write_recipe(recipes_dir.path(), "probe.json", minimal_recipe("minimal.recipe"));

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

    REQUIRE(loaded.has_value());
    const recipes::Recipe& value = loaded.value();
    check_text(value.recipe_id, "minimal.recipe", "recipe_id");
    check_text(value.algorithm, "example.threshold", "algorithm");
    CHECK(value.parameters.is_object());
    CHECK(value.parameters.empty());
    CHECK_FALSE(value.capture.width.has_value());
    CHECK_FALSE(value.capture.height.has_value());
    CHECK_FALSE(value.capture.frame_rate.has_value());
    CHECK_FALSE(value.capture.pixel_format.has_value());
    check_integer(value.capture.settle_frames, 0, "capture.settle_frames");
    CHECK(value.artifacts.required == false);
}

TEST_CASE("CVF-004 B1: every documented save_policy token loads",
          "[cvf-004][B1][recipe][boundary]")
{
    const auto registry = make_compiled_registry();
    for (const char* policy : {"always", "fail_or_error", "never"}) {
        Json recipe = minimal_recipe("policy.recipe");
        recipe["artifacts"]["save_policy"] = policy;
        INFO("save_policy: " << policy);
        expect_recipe_ok(*registry, recipe);
    }
}

TEST_CASE("CVF-004 B1: recipe parameters are validated through the registered algorithm and "
          "preserved exactly",
          "[cvf-004][B1][recipe]")
{
    const auto stub = make_stub_registry("cvf004.stub.accept", false);
    const Json rich_parameters = Json{
        {"nested", Json{{"list", Json::array({1, 2, 3})}, {"flag", true}}},
        {"value", 0.25},
    };
    Json recipe = minimal_recipe("stub.recipe", "cvf004.stub.accept");
    recipe["parameters"] = rich_parameters;

    TempDir recipes_dir("recipe_rich");
    write_recipe(recipes_dir.path(), "probe.json", recipe);

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *stub.registry);

    REQUIRE(loaded.has_value());
    CHECK(loaded.value().parameters == rich_parameters);
    CHECK(stub.algorithm->validate_calls() == 1);
}

TEST_CASE("CVF-004 B3 negative: a missing top-level key is rejected as recipe_value_invalid",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();
    for (const char* key : {"schema_version", "recipe_id", "algorithm", "parameters", "capture",
                            "artifacts"}) {
        Json recipe = valid_recipe("probe.recipe");
        REQUIRE(recipe.contains(key));
        recipe.erase(key);
        INFO("missing key: " << key);
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_value_invalid);
    }
}

TEST_CASE("CVF-004 B3 negative: an unknown top-level key is rejected as recipe_unknown_key",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    SECTION("well-formed JSON")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["extra"] = 1;
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_unknown_key);
    }

    SECTION("raw text")
    {
        const std::string text = replace_once(valid_recipe_json_text(), R"("schema_version": 1,)",
                                              R"("schema_version": 1, "extra": 1,)");
        expect_recipe_text_failure(*registry, text, core::Status::config_error,
                                   core::ErrorCode::recipe_unknown_key);
    }
}

TEST_CASE("CVF-004 B3 negative: duplicate keys are rejected at top level and nested levels",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    SECTION("top-level duplicate")
    {
        const std::string text = replace_once(valid_recipe_json_text(), R"("schema_version": 1,)",
                                              R"("schema_version": 1, "schema_version": 1,)");
        expect_recipe_text_failure(*registry, text, core::Status::config_error,
                                   core::ErrorCode::recipe_duplicate_key);
    }

    SECTION("duplicate inside parameters")
    {
        const std::string text = replace_once(valid_recipe_json_text(), R"("threshold": 128)",
                                              R"("threshold": 128, "threshold": 128)");
        expect_recipe_text_failure(*registry, text, core::Status::config_error,
                                   core::ErrorCode::recipe_duplicate_key);
    }

    SECTION("duplicate inside capture")
    {
        const std::string text = replace_once(valid_recipe_json_text(), R"("settle_frames": 3)",
                                              R"("settle_frames": 3, "settle_frames": 3)");
        expect_recipe_text_failure(*registry, text, core::Status::config_error,
                                   core::ErrorCode::recipe_duplicate_key);
    }
}

TEST_CASE("CVF-004 B3 negative: malformed JSON reports recipe_parse_error",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    const std::string text = replace_once(valid_recipe_json_text(), R"("settle_frames": 3)",
                                          R"("settle_frames": )");
    expect_recipe_text_failure(*registry, text, core::Status::config_error,
                               core::ErrorCode::recipe_parse_error);
}

TEST_CASE("CVF-004 B3 negative: a missing recipe file reports recipe_file_missing",
          "[cvf-004][B3][recipe][negative]")
{
    TempDir recipes_dir("recipe_missing_file");
    const auto registry = make_compiled_registry();

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "absent.json", *registry);

    check_failure(loaded, core::Status::config_error, core::ErrorCode::recipe_file_missing);
}

TEST_CASE("CVF-004 B3 negative: schema_version values other than 1 are rejected",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    SECTION("schema_version 0")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["schema_version"] = 0;
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_schema_version);
    }

    SECTION("schema_version 2")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["schema_version"] = 2;
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_schema_version);
    }
}

TEST_CASE("CVF-004 B3 negative: invalid recipe_id values are rejected as recipe_id_invalid",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();
    const std::string too_long(129, 'a');

    SECTION("empty")
    {
        expect_recipe_failure(*registry, valid_recipe(""), core::Status::config_error,
                              core::ErrorCode::recipe_id_invalid);
    }

    SECTION("longer than 128 bytes")
    {
        expect_recipe_failure(*registry, valid_recipe(too_long), core::Status::config_error,
                              core::ErrorCode::recipe_id_invalid);
    }

    SECTION("characters outside the documented alphabet")
    {
        for (const char* recipe_id : {"bad id", "bad/id", "bad:id", "bad*id", "bad?id"}) {
            INFO("recipe_id: [" << recipe_id << "]");
            expect_recipe_failure(*registry, valid_recipe(recipe_id), core::Status::config_error,
                                  core::ErrorCode::recipe_id_invalid);
        }
    }
}

TEST_CASE("CVF-004 B3 boundary: a 128-byte recipe_id is accepted and a 129-byte one is rejected",
          "[cvf-004][B3][recipe][boundary]")
{
    const auto registry = make_compiled_registry();
    const std::string id_128(128, 'a');
    TempDir recipes_dir("recipe_id_boundary");
    write_recipe(recipes_dir.path(), "probe.json", valid_recipe(id_128));

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

    REQUIRE(loaded.has_value());
    CHECK(loaded.value().recipe_id == id_128);

    expect_recipe_failure(*registry, valid_recipe(std::string(129, 'a')), core::Status::config_error,
                          core::ErrorCode::recipe_id_invalid);
}

TEST_CASE("CVF-004 B3 negative: unknown or malformed algorithm keys are rejected as "
          "recipe_algorithm_unknown",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    SECTION("well-formed but unknown")
    {
        expect_recipe_failure(*registry, valid_recipe("probe.recipe", "missing.algorithm"),
                              core::Status::config_error,
                              core::ErrorCode::recipe_algorithm_unknown);
    }

    SECTION("malformed keys")
    {
        for (const char* algorithm : {"Bad.Key", "bad key", "bad/key", "bad:key"}) {
            INFO("algorithm: [" << algorithm << "]");
            expect_recipe_failure(*registry, valid_recipe("probe.recipe", algorithm),
                                  core::Status::config_error,
                                  core::ErrorCode::recipe_algorithm_unknown);
        }
    }

    SECTION("longer than 64 characters")
    {
        expect_recipe_failure(*registry, valid_recipe("probe.recipe", std::string(65, 'a')),
                              core::Status::config_error,
                              core::ErrorCode::recipe_algorithm_unknown);
    }

    SECTION("not a string")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["algorithm"] = 7;
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_algorithm_unknown);
    }
}

TEST_CASE("CVF-004 B3 boundary: a 64-character registered algorithm key is accepted",
          "[cvf-004][B3][recipe][boundary]")
{
    const std::string key_64(64, 'a');
    const auto stub = make_stub_registry(key_64, false);
    TempDir recipes_dir("recipe_algorithm_boundary");
    write_recipe(recipes_dir.path(), "probe.json", valid_recipe("probe.recipe", key_64));

    auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *stub.registry);

    REQUIRE(loaded.has_value());
    check_text(loaded.value().algorithm, key_64, "algorithm");
}

TEST_CASE("CVF-004 B3 negative: parameters rejected by the algorithm are reported as "
          "recipe_parameters_invalid",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    SECTION("example.threshold rejects an out-of-range threshold")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["parameters"] = Json{{"threshold", 256}};
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_parameters_invalid);
    }

    SECTION("example.threshold rejects an unknown parameter key")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["parameters"] = Json{{"bogus", 1}};
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_parameters_invalid);
    }

    SECTION("a rejecting registered algorithm is consulted")
    {
        const auto stub = make_stub_registry("cvf004.stub.reject", true);
        TempDir recipes_dir("recipe_stub_reject");
        write_recipe(recipes_dir.path(), "probe.json",
                     minimal_recipe("probe.recipe", "cvf004.stub.reject"));

        auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *stub.registry);

        check_failure(loaded, core::Status::config_error,
                      core::ErrorCode::recipe_parameters_invalid);
        CHECK(stub.algorithm->validate_calls() == 1);
    }
}

TEST_CASE("CVF-004 B3 negative: non-object parameters are rejected",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    for (const char* kind : {"array", "number", "string", "null"}) {
        Json recipe = valid_recipe("probe.recipe");
        if (std::string_view(kind) == "array") {
            recipe["parameters"] = Json::array();
        } else if (std::string_view(kind) == "number") {
            recipe["parameters"] = 7;
        } else if (std::string_view(kind) == "string") {
            recipe["parameters"] = "threshold";
        } else {
            recipe["parameters"] = nullptr;
        }
        INFO("parameters kind: " << kind);
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_parameters_invalid);
    }
}

TEST_CASE("CVF-004 B3 negative: capture errors are reported as recipe_capture_invalid",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    SECTION("missing settle_frames")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["capture"].erase("settle_frames");
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_capture_invalid);
    }

    SECTION("settle_frames out of range or wrong type")
    {
        for (const Json& value : {Json(1001), Json(-1), Json("3"), Json(3.5)}) {
            Json recipe = valid_recipe("probe.recipe");
            recipe["capture"]["settle_frames"] = value;
            INFO("settle_frames: " << value.dump());
            expect_recipe_failure(*registry, recipe, core::Status::config_error,
                                  core::ErrorCode::recipe_capture_invalid);
        }
    }

    SECTION("unknown capture key")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["capture"]["exposure"] = 1;
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_capture_invalid);
    }

    SECTION("capture is not an object")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["capture"] = Json::array();
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_capture_invalid);
    }

    SECTION("capture width and height out of range")
    {
        for (const int value : {0, 16385}) {
            Json recipe = valid_recipe("probe.recipe");
            recipe["capture"]["width"] = value;
            INFO("width: " << value);
            expect_recipe_failure(*registry, recipe, core::Status::config_error,
                                  core::ErrorCode::recipe_capture_invalid);
        }
        for (const int value : {0, 16385}) {
            Json recipe = valid_recipe("probe.recipe");
            recipe["capture"]["height"] = value;
            INFO("height: " << value);
            expect_recipe_failure(*registry, recipe, core::Status::config_error,
                                  core::ErrorCode::recipe_capture_invalid);
        }
    }

    SECTION("capture frame_rate out of range")
    {
        for (const double value : {0.0, -1.0, 1000.5}) {
            Json recipe = valid_recipe("probe.recipe");
            recipe["capture"]["frame_rate"] = value;
            INFO("frame_rate: " << value);
            expect_recipe_failure(*registry, recipe, core::Status::config_error,
                                  core::ErrorCode::recipe_capture_invalid);
        }
    }

    SECTION("capture pixel_format token invalid")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["capture"]["pixel_format"] = "yuv420";
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_capture_invalid);
    }
}

TEST_CASE("CVF-004 B3 boundary: capture overrides are inclusive at their documented bounds",
          "[cvf-004][B3][recipe][boundary]")
{
    const auto registry = make_compiled_registry();

    SECTION("settle_frames 0 and 1000 are accepted")
    {
        for (const int value : {0, 1000}) {
            Json recipe = valid_recipe("probe.recipe");
            recipe["capture"]["settle_frames"] = value;
            INFO("settle_frames: " << value);
            TempDir recipes_dir("recipe_settle_boundary");
            write_recipe(recipes_dir.path(), "probe.json", recipe);
            auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);
            REQUIRE(loaded.has_value());
            check_integer(loaded.value().capture.settle_frames, value, "settle_frames");
        }
    }

    SECTION("width and height 1 and 16384 are accepted")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["capture"]["width"] = 1;
        recipe["capture"]["height"] = 16384;
        TempDir recipes_dir("recipe_size_boundary");
        write_recipe(recipes_dir.path(), "probe.json", recipe);

        auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

        REQUIRE(loaded.has_value());
        REQUIRE(loaded.value().capture.width.has_value());
        REQUIRE(loaded.value().capture.height.has_value());
        check_integer(*loaded.value().capture.width, 1, "capture.width");
        check_integer(*loaded.value().capture.height, 16384, "capture.height");
    }

    SECTION("frame_rate just above zero is accepted")
    {
        // Verify-phase requirement-derived addition: the capture frame_rate
        // override shares the documented (0, 1000] bound.
        Json recipe = valid_recipe("probe.recipe");
        recipe["capture"]["frame_rate"] = 0.001;
        TempDir recipes_dir("recipe_fps_epsilon");
        write_recipe(recipes_dir.path(), "probe.json", recipe);

        auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

        REQUIRE(loaded.has_value());
        REQUIRE(loaded.value().capture.frame_rate.has_value());
        check_number(*loaded.value().capture.frame_rate, 0.001, "capture.frame_rate");
    }
}

TEST_CASE("CVF-004 B3 negative: artifacts errors are reported as recipe_artifacts_invalid",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    SECTION("unknown save_policy token")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["artifacts"]["save_policy"] = "sometimes";
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_artifacts_invalid);
    }

    SECTION("missing save_policy")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["artifacts"].erase("save_policy");
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_artifacts_invalid);
    }

    SECTION("missing required")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["artifacts"].erase("required");
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_artifacts_invalid);
    }

    SECTION("required is not a boolean")
    {
        for (const Json& value : {Json("yes"), Json(1)}) {
            Json recipe = valid_recipe("probe.recipe");
            recipe["artifacts"]["required"] = value;
            INFO("required: " << value.dump());
            expect_recipe_failure(*registry, recipe, core::Status::config_error,
                                  core::ErrorCode::recipe_artifacts_invalid);
        }
    }

    SECTION("unknown artifacts key")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["artifacts"]["path"] = "captures";
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_artifacts_invalid);
    }

    SECTION("artifacts is not an object")
    {
        Json recipe = valid_recipe("probe.recipe");
        recipe["artifacts"] = Json::array();
        expect_recipe_failure(*registry, recipe, core::Status::config_error,
                              core::ErrorCode::recipe_artifacts_invalid);
    }
}

TEST_CASE("CVF-004 B3 negative: invalid UTF-8 in string values is rejected",
          "[cvf-004][B3][recipe][negative]")
{
    const auto registry = make_compiled_registry();

    SECTION("recipe_id")
    {
        TempDir recipes_dir("recipe_utf8_id");
        const std::string text = replace_once(valid_recipe_json_text(), R"("probe.recipe")",
                                              "\"pro"
                                              "\xC3\x28"
                                              "be\"");
        write_text(recipes_dir.path() / "probe.json", text);

        auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.failure().status == core::Status::config_error);
        // The brief requires rejection; nlohmann rejects invalid UTF-8 during
        // parsing, so either the parse or the value classification is acceptable.
        CHECK((loaded.failure().code == core::ErrorCode::recipe_parse_error ||
               loaded.failure().code == core::ErrorCode::recipe_value_invalid ||
               loaded.failure().code == core::ErrorCode::recipe_id_invalid));
    }

    SECTION("algorithm")
    {
        TempDir recipes_dir("recipe_utf8_algorithm");
        const std::string text = replace_once(valid_recipe_json_text(), R"("example.threshold")",
                                              "\"exa"
                                              "\xC3\x28"
                                              "mple\"");
        write_text(recipes_dir.path() / "probe.json", text);

        auto loaded = recipes::load_recipe_file(recipes_dir.path() / "probe.json", *registry);

        REQUIRE_FALSE(loaded.has_value());
        CHECK(loaded.failure().status == core::Status::config_error);
        CHECK((loaded.failure().code == core::ErrorCode::recipe_parse_error ||
               loaded.failure().code == core::ErrorCode::recipe_value_invalid ||
               loaded.failure().code == core::ErrorCode::recipe_algorithm_unknown));
    }
}

#ifndef _WIN32
TEST_CASE("CVF-004 B3 negative: an unreadable recipe file reports recipe_io_error",
          "[cvf-004][B3][recipe][negative]")
{
    if (::geteuid() == 0) {
        SKIP("permission bits are not enforced for the superuser");
    }

    TempDir recipes_dir("recipe_unreadable");
    const auto registry = make_compiled_registry();
    const auto recipe_file = recipes_dir.path() / "probe.json";
    write_recipe(recipes_dir.path(), "probe.json", minimal_recipe("probe.recipe"));
    std::filesystem::permissions(recipe_file, std::filesystem::perms::none);

    auto loaded = recipes::load_recipe_file(recipe_file, *registry);

    check_failure(loaded, core::Status::config_error, core::ErrorCode::recipe_io_error);

    std::filesystem::permissions(recipe_file, std::filesystem::perms::owner_all);
}
#endif

// CVF-106 independent black-box tests: catalog/reload asset lifecycle.
//
// Covers test-brief B1 (a recipe whose preparation fails rejects the whole
// catalog and the previous snapshot stays active), B4 (assets decoded once; a
// removed asset file does not change inspect output), B7 (reload with a new
// template changes inspect only after the atomic swap), and B8 (example.threshold
// keeps its observable output through the new lifecycle). The suite drives the
// runtime through the frozen Context API and the deterministic file/synthetic
// test backends only. No production .cpp is read or included.

#include "cvf106_test.helpers.h"

#include <cmath>
#include <limits>

namespace {

using namespace cvf106;

constexpr int k_frame_width = 64;
constexpr int k_frame_height = 48;
constexpr int k_template_x = 20;
constexpr int k_template_y = 14;
constexpr double k_passing_threshold = 0.8;
constexpr std::size_t k_oversized_asset_bytes = 16777217u;
constexpr const char* k_recipe_id = "tmpl.recipe";

Json recipe_assets(std::string reference)
{
    return Json{{"tmpl", std::move(reference)}};
}

Json full_roi_parameters(double threshold)
{
    return template_parameters(0, 0, k_frame_width, k_frame_height, threshold);
}

// Writes a complete, valid template.match config root plus the served frame.
void write_valid_site(const std::filesystem::path& root, const std::filesystem::path& frame_file,
                      const cv::Mat& tmpl, int origin_x, int origin_y, double threshold)
{
    const Json parameters = full_roi_parameters(threshold);
    const Json recipe =
        template_recipe(k_recipe_id, parameters, k_frame_width, k_frame_height,
                        recipe_assets(std::string(k_asset_reference)));
    write_config_root(root, base_config(k_frame_width, k_frame_height), k_recipe_id, recipe,
                      {{std::string(k_asset_reference), tmpl}});
    write_png(frame_file, frame_with_template(k_frame_width, k_frame_height, tmpl, origin_x,
                                              origin_y));
}

rt::RuntimeOptions site_options(const std::filesystem::path& root,
                                const std::filesystem::path& output_root,
                                const std::filesystem::path& frame_file)
{
    rt::RuntimeOptions options = runtime_options(root, output_root);
    set_file_camera(options, frame_file, 16);
    return options;
}

rt::InspectionOutcome inspect_ok(const std::unique_ptr<rt::Context>& context)
{
    auto outcome = context->inspect(inspection_request(k_recipe_id, 5000));
    REQUIRE(outcome.has_value());
    return outcome.value();
}

void check_passing_at(const std::unique_ptr<rt::Context>& context, int expected_x, int expected_y)
{
    const rt::InspectionOutcome outcome = inspect_ok(context);
    REQUIRE(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::pass);
    CHECK(outcome_score(outcome) >= k_passing_threshold);
    CHECK(outcome_int(outcome, "x") == expected_x);
    CHECK(outcome_int(outcome, "y") == expected_y);
}

}  // namespace

TEST_CASE("CVF-106 B1 a failing recipe rejects catalog creation", "[cvf106]")
{
    const cv::Mat tmpl = asymmetric_template();

    SECTION("missing asset")
    {
        TempDir root("missing");
        TempDir output("missing_out");
        const Json recipe =
            template_recipe(k_recipe_id, full_roi_parameters(k_passing_threshold), k_frame_width,
                            k_frame_height, recipe_assets("missing.png"));
        write_config_root(root.path(), base_config(k_frame_width, k_frame_height), k_recipe_id,
                          recipe);
        rt::RuntimeOptions options =
            site_options(root.path(), output.path(), root.path() / "frame.png");
        set_file_camera(options, root.path() / "frame.png", 4);
        auto created = rt::Context::create(options);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.failure().status == core::Status::config_error);
    }

    SECTION("escaping asset reference")
    {
        TempDir root("escape");
        TempDir output("escape_out");
        // The escaping reference resolves to <root>/escape.png, which exists and
        // is a valid PNG, so only the escape rule can reject it.
        write_png(root.path() / "escape.png", tmpl);
        const Json recipe =
            template_recipe(k_recipe_id, full_roi_parameters(k_passing_threshold), k_frame_width,
                            k_frame_height, recipe_assets("../escape.png"));
        write_config_root(root.path(), base_config(k_frame_width, k_frame_height), k_recipe_id,
                          recipe);
        rt::RuntimeOptions options =
            site_options(root.path(), output.path(), root.path() / "frame.png");
        auto created = rt::Context::create(options);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.failure().status == core::Status::config_error);
    }

    SECTION("invalid ROI")
    {
        TempDir root("bad_roi");
        TempDir output("bad_roi_out");
        const Json recipe =
            template_recipe(k_recipe_id, template_parameters(0, 0, 0, 0, k_passing_threshold),
                            k_frame_width, k_frame_height, recipe_assets(std::string(k_asset_reference)));
        write_config_root(root.path(), base_config(k_frame_width, k_frame_height), k_recipe_id,
                          recipe, {{std::string(k_asset_reference), tmpl}});
        rt::RuntimeOptions options =
            site_options(root.path(), output.path(), root.path() / "frame.png");
        auto created = rt::Context::create(options);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.failure().status == core::Status::config_error);
    }

    SECTION("zero-variance template")
    {
        TempDir root("zero_var");
        TempDir output("zero_var_out");
        const Json recipe =
            template_recipe(k_recipe_id, full_roi_parameters(k_passing_threshold), k_frame_width,
                            k_frame_height, recipe_assets(std::string(k_asset_reference)));
        write_config_root(root.path(), base_config(k_frame_width, k_frame_height), k_recipe_id,
                          recipe, {{std::string(k_asset_reference), uniform_template(5, 4, 200)}});
        rt::RuntimeOptions options =
            site_options(root.path(), output.path(), root.path() / "frame.png");
        auto created = rt::Context::create(options);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.failure().status == core::Status::config_error);
    }

    SECTION("oversized asset")
    {
        TempDir root("oversized");
        TempDir output("oversized_out");
        write_raw_asset(root.path(), k_asset_reference,
                        std::string(k_oversized_asset_bytes, 'x'));
        const Json recipe =
            template_recipe(k_recipe_id, full_roi_parameters(k_passing_threshold), k_frame_width,
                            k_frame_height, recipe_assets(std::string(k_asset_reference)));
        write_config_root(root.path(), base_config(k_frame_width, k_frame_height), k_recipe_id,
                          recipe);
        rt::RuntimeOptions options =
            site_options(root.path(), output.path(), root.path() / "frame.png");
        auto created = rt::Context::create(options);
        REQUIRE_FALSE(created.has_value());
        CHECK(created.failure().status == core::Status::config_error);
    }
}

TEST_CASE("CVF-106 B1 a failed reload keeps the previous snapshot active", "[cvf106]")
{
    TempDir root("reload_keep");
    TempDir output("reload_keep_out");
    const cv::Mat tmpl = asymmetric_template();
    write_valid_site(root.path(), root.path() / "frame.png", tmpl, k_template_x, k_template_y,
                     k_passing_threshold);

    auto context = create_context(site_options(root.path(), output.path(), root.path() / "frame.png"));
    check_passing_at(context, k_template_x, k_template_y);

    // Corrupt the on-disk recipe with a missing asset; the reload must fail and
    // the old prepared snapshot must keep serving.
    const Json broken =
        template_recipe(k_recipe_id, full_roi_parameters(k_passing_threshold), k_frame_width,
                        k_frame_height, recipe_assets("now_missing.png"));
    write_json(root.path() / "recipes" / "tmpl.recipe.json", broken);
    auto reloaded = context->reload_recipes();
    REQUIRE_FALSE(reloaded.has_value());
    CHECK(reloaded.failure().status == core::Status::config_error);

    check_passing_at(context, k_template_x, k_template_y);
}

TEST_CASE("CVF-106 B4 removing the asset after load does not change inspect output", "[cvf106]")
{
    TempDir root("asset_gone");
    TempDir output("asset_gone_out");
    const cv::Mat tmpl = asymmetric_template();
    write_valid_site(root.path(), root.path() / "frame.png", tmpl, k_template_x, k_template_y,
                     k_passing_threshold);

    auto context = create_context(site_options(root.path(), output.path(), root.path() / "frame.png"));
    check_passing_at(context, k_template_x, k_template_y);

    std::error_code error;
    std::filesystem::remove(root.path() / "assets" / std::string(k_asset_reference), error);
    REQUIRE_FALSE(std::filesystem::exists(root.path() / "assets" / std::string(k_asset_reference)));

    check_passing_at(context, k_template_x, k_template_y);
}

TEST_CASE("CVF-106 B7 reload swaps to a new template only after a successful candidate",
          "[cvf106]")
{
    TempDir root("swap");
    TempDir output("swap_out");
    const cv::Mat first = asymmetric_template();
    write_valid_site(root.path(), root.path() / "frame.png", first, k_template_x, k_template_y,
                     k_passing_threshold);

    auto context = create_context(site_options(root.path(), output.path(), root.path() / "frame.png"));
    check_passing_at(context, k_template_x, k_template_y);

    // A failed candidate (invalid ROI) must not change the active snapshot.
    const Json broken =
        template_recipe(k_recipe_id, template_parameters(0, 0, 0, 0, k_passing_threshold),
                        k_frame_width, k_frame_height, recipe_assets(std::string(k_asset_reference)));
    write_json(root.path() / "recipes" / "tmpl.recipe.json", broken);
    auto failed = context->reload_recipes();
    REQUIRE_FALSE(failed.has_value());
    check_passing_at(context, k_template_x, k_template_y);

    // Rewrite both the asset and the served frame, then a successful reload must
    // switch the observed match to the new template/location.
    const cv::Mat second = asymmetric_template_alt();
    write_valid_site(root.path(), root.path() / "frame.png", second, 33, 7, k_passing_threshold);
    auto reloaded = context->reload_recipes();
    REQUIRE(reloaded.has_value());
    check_passing_at(context, 33, 7);
}

TEST_CASE("CVF-106 B8 example.threshold keeps its observable output through the lifecycle",
          "[cvf106]")
{
    TempDir root("example");
    TempDir output("example_out");
    const Json recipe = example_recipe("example.recipe", k_frame_width, k_frame_height);
    write_config_root(root.path(), base_config(k_frame_width, k_frame_height), "example.recipe",
                      recipe);

    // A fixed single-frame file camera serves byte-identical pixels on every
    // capture, unlike the sequence-dependent synthetic backend whose pixel is
    // f(x, y, sequence); with a constant frame the serialized output is
    // genuinely deterministic across in-process inspects and across a reload.
    const std::filesystem::path frame_file = root.path() / "example_frame.png";
    write_png(frame_file, frame_without_template(k_frame_width, k_frame_height));

    rt::RuntimeOptions options = runtime_options(root.path(), output.path());
    set_file_camera(options, frame_file, 4);
    auto context = create_context(options);

    auto first = context->inspect(inspection_request("example.recipe", 5000));
    REQUIRE(first.has_value());
    REQUIRE(first.value().status == core::Status::ok);
    CHECK(first.value().verdict == core::Verdict::pass);

    // Stable structural observables: the example algorithm's measurements are
    // flattened into the top-level final result object (final_result_payload_v1,
    // assembled from measurements and optional defects); white_pixels,
    // total_pixels, and pass_ratio are top-level members and every numeric
    // measurement is finite.
    const Json first_object = outcome_measurements(first.value());
    REQUIRE(first_object.is_object());
    CHECK(first_object.contains("white_pixels"));
    CHECK(first_object.contains("total_pixels"));
    CHECK(first_object.contains("pass_ratio"));
    CHECK(first_object.at("white_pixels").is_number_integer());
    CHECK(first_object.at("total_pixels").is_number_integer());
    CHECK(first_object.at("pass_ratio").is_number());
    CHECK(std::isfinite(first_object.at("pass_ratio").get<double>()));
    for (const auto& item : first_object.items()) {
        if (item.value().is_number()) {
            CHECK(std::isfinite(item.value().get<double>()));
        }
    }

    auto second = context->inspect(inspection_request("example.recipe", 5000));
    REQUIRE(second.has_value());
    CHECK(second.value().status == first.value().status);
    CHECK(second.value().verdict == first.value().verdict);
    CHECK(outcome_json(second.value()) == outcome_json(first.value()));
    CHECK(second.value().output_text == first.value().output_text);

    auto reloaded = context->reload_recipes();
    REQUIRE(reloaded.has_value());

    auto third = context->inspect(inspection_request("example.recipe", 5000));
    REQUIRE(third.has_value());
    CHECK(third.value().status == first.value().status);
    CHECK(third.value().verdict == first.value().verdict);
    CHECK(outcome_json(third.value()) == outcome_json(first.value()));
    CHECK(third.value().output_text == first.value().output_text);
}

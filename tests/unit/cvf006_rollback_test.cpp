// CVF-006 independent black-box tests: rollback-safe initialization (brief B11).
#include <filesystem>

#include "cvf006_test_support.h"

using namespace cvf006;

TEST_CASE("CVF-006 B11: a failed initialization leaves no live context and a later valid "
          "initialization succeeds",
          "[cvf-006][B11]")
{
    TempDir config_root("b11_config");
    TempDir output_root("b11_output");
    TempDir scratch("b11_scratch");

    // A file-backed camera whose frames directory does not exist cannot be
    // resolved, so initialization fails at the camera stage, after the config
    // and diagnostics stages.
    const std::filesystem::path missing_frames = scratch.path() / "missing_frames_dir";
    REQUIRE_FALSE(std::filesystem::exists(missing_frames));

    write_config(config_root.path(), file_config(missing_frames));
    write_recipe(config_root.path(), "recipe.json", recipe_document("example.pass", pass_parameters()));

    auto failing_options = runtime_options(config_root.path(), output_root.path());
    auto failed = rt::Context::create(failing_options);
    REQUIRE_FALSE(failed.has_value());

    const core::Status failure_status = failed.failure().status;
    CHECK((failure_status == core::Status::camera_not_found ||
           failure_status == core::Status::camera_io ||
           failure_status == core::Status::config_error));

    // A repeated failed attempt must not be blocked by a leaked live-context
    // slot either.
    auto failed_again = rt::Context::create(failing_options);
    REQUIRE_FALSE(failed_again.has_value());

    // The rollback left the process able to initialize a healthy context.
    SyntheticHarness valid("b11_valid", "example.pass", pass_parameters());
    const RequestHolder request("example.pass", "b11-valid", 5000u);
    const rt::InspectionOutcome outcome = valid.inspect(request.get());

    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::pass);
}

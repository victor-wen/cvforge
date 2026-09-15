// CVF-006 independent black-box tests: degraded optional file-log sink (brief B10).
#include <filesystem>
#include <fstream>

#include "cvf006_test_support.h"

using namespace cvf006;

TEST_CASE("CVF-006 B10: a blocked logs path degrades to warning bit 1 and inspect still runs OK",
          "[cvf-006][B10][warnings]")
{
    TempDir config_root("b10_config");
    TempDir output_root("b10_output");

    write_config(config_root.path(), synthetic_config());
    write_recipe(config_root.path(), "recipe.json", recipe_document("example.pass", pass_parameters()));

    // Block the file-log sink location with a regular file so directory
    // creation below it cannot succeed. Both common spellings are blocked;
    // whichever one the runtime uses, the optional sink fails while
    // initialization must keep going.
    for (const char* name : {"logs", "log"}) {
        const std::filesystem::path blocked = output_root.path() / name;
        std::ofstream blocker(blocked, std::ios::binary | std::ios::trunc);
        REQUIRE(blocker.good());
        blocker << "cvf006 blocked log path";
        REQUIRE(blocker.good());
    }

    auto options = runtime_options(config_root.path(), output_root.path(), true);
    auto backend = make_synthetic_backend();
    set_camera_override(options, backend);
    auto context = create_context(options);

    const RequestHolder request("example.pass", "b10-blocked-logs", 5000u);
    const rt::InspectionOutcome outcome = inspect_with(context, request.get());

    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::pass);
    CHECK((warning_bits(outcome.warning_flags) & kLogSinkWarningBit) != 0u);
    CHECK((warning_bits(context->warning_flags()) & kLogSinkWarningBit) != 0u);
}

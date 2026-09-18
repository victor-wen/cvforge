// CVF-105 independent black-box tests: the runtime's canonical pre-serialized
// result payload.
//
// Brief B6/B7: one final object is serialized exactly once and retained as a
// string; InspectionOutcome::output_text is that canonical string and equals
// the internal object with no truncation or reordering, within the 65535-byte
// payload bound. Driven end to end through the internal runtime context with
// the deterministic synthetic backend (test-enabled builds only).
//
// Production surface used (frozen headers only): src/runtime/context.h and the
// public bound constant; no production .cpp is read.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <string>

#include "cvf105_test.helpers.h"

#include "core/status.h"
#include "cvforwin/cvf_api.h"
#include "runtime/context.h"

namespace {

namespace core = cvforwin::core;
namespace rt = cvforwin::runtime;

using cvf105::Json;
using cvf105::TempDir;

}  // namespace

TEST_CASE("CVF-105 B7: output_text is the canonical serialization of the internal result object",
          "[cvf-105][B7][runtime]")
{
    TempDir root("runtime_output_text");
    cvf105::write_config_root(root.path(), cvf105::synthetic_config_json());

    rt::RuntimeOptions options;
    options.config_root = root.path();
    options.output_root = root.path() / "out";
    options.enable_file_logging = false;
    options.enable_callback_logging = false;

    auto created = rt::Context::create(options);
    REQUIRE(created.has_value());
    std::unique_ptr<rt::Context> context = std::move(created.value());

    rt::InspectionRequest request;
    request.recipe_id = "example";
    request.request_id = "b7-runtime";
    request.timeout_ms = rt::TimeoutMs(0u);

    auto inspected = context->inspect(request);
    REQUIRE(inspected.has_value());

    const rt::InspectionOutcome& outcome = inspected.value();
    CHECK(outcome.status == core::Status::ok);
    REQUIRE_FALSE(outcome.output_text.empty());
    CHECK(outcome.output_text == outcome.output_json.dump());
    CHECK(outcome.output_text.size() == outcome.output_json.dump().size());
    CHECK(outcome.output_text.size() <= rt::k_max_result_payload_bytes);
    CHECK(outcome.output_text.size() < CVF_RESULT_JSON_REQUIRED_CAPACITY);
    CHECK(Json::parse(outcome.output_text) == outcome.output_json);
}

/* ------------------------------------------------------------------------- */
/* B7 edge: a FAIL result's internal object round-trips byte-exactly, with no  */
/* truncation or reordering.                                                   */
/*                                                                            */
/* The default example algorithm emits its measurements as a flat JSON object  */
/* and omits an empty defects array, so this case pins the exact round-trip    */
/* for that real runtime shape. The wrapped measurements-plus-defects shape is */
/* covered at the serialization seam in cvf105_payload_test.cpp.              */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B7 edge: output_text round-trips a FAIL result's internal object exactly",
          "[cvf-105][B7][runtime]")
{
    TempDir root("runtime_output_text_defects");
    const Json config =
        cvf105::base_config_json("synthetic", "synthetic0", "0000", "0000", "Synthetic camera");
    cvf105::write_text(root.path() / "cvforwin.json", config.dump(2));

    // threshold 255 with a required full pass ratio makes the deterministic
    // synthetic frame FAIL, so the algorithm emits its defect information.
    Json recipe = cvf105::base_recipe_json("failing");
    recipe["parameters"]["threshold"] = 255;
    recipe["parameters"]["min_pass_ratio"] = 1.0;
    cvf105::write_text(root.path() / "recipes" / "failing.json", recipe.dump(2));

    rt::RuntimeOptions options;
    options.config_root = root.path();
    options.output_root = root.path() / "out";
    options.enable_file_logging = false;
    options.enable_callback_logging = false;

    auto created = rt::Context::create(options);
    REQUIRE(created.has_value());
    std::unique_ptr<rt::Context> context = std::move(created.value());

    rt::InspectionRequest request;
    request.recipe_id = "failing";
    request.request_id = "b7-runtime-defects";
    request.timeout_ms = rt::TimeoutMs(0u);

    auto inspected = context->inspect(request);
    REQUIRE(inspected.has_value());
    const rt::InspectionOutcome& outcome = inspected.value();

    INFO("output_json: " << outcome.output_json.dump());
    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::fail);
    REQUIRE(outcome.output_json.is_object());
    CHECK_FALSE(outcome.output_json.empty());
    CHECK(outcome.output_json.contains("white_pixels"));
    CHECK(outcome.output_json.contains("total_pixels"));
    CHECK(outcome.output_json.contains("pass_ratio"));

    // The canonical pre-serialized payload must equal the internal object
    // byte-for-byte, for the FAIL object as well as the PASS object.
    CHECK(outcome.output_text == outcome.output_json.dump());
    CHECK(outcome.output_text.size() == outcome.output_json.dump().size());
    CHECK(outcome.output_text.size() <= rt::k_max_result_payload_bytes);
    CHECK(outcome.output_text.size() < CVF_RESULT_JSON_REQUIRED_CAPACITY);
    CHECK(Json::parse(outcome.output_text) == outcome.output_json);
}

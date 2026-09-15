// CVF-006 independent black-box tests: one bounded reconnect/recapture (brief B8).
#include <chrono>

#include "cvf006_test_support.h"

using namespace cvf006;

TEST_CASE("CVF-006 B8: a transient disconnect yields OK with exactly 2 captures and 1 reconnect",
          "[cvf-006][B8]")
{
    SyntheticHarness harness("b8_transient", "example.pass", pass_parameters());
    harness.backend()->inject_capture_failure(1, cam::FaultKind::disconnect);

    const RequestHolder request("example.pass", "b8-transient", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::pass);
    CHECK(harness.backend()->capture_call_count == 2);
    CHECK(harness.backend()->reconnect_call_count == 1);
}

TEST_CASE("CVF-006 B8: faults on both attempts yield CAMERA_IO after exactly 2 captures and 1 "
          "reconnect",
          "[cvf-006][B8]")
{
    SyntheticHarness harness("b8_double", "example.pass", pass_parameters());
    harness.backend()->inject_capture_failure(1, cam::FaultKind::disconnect);
    harness.backend()->inject_capture_failure(2, cam::FaultKind::capture_error);

    const RequestHolder request("example.pass", "b8-double", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    CHECK(outcome.status == core::Status::camera_io);
    CHECK(outcome.verdict == core::Verdict::not_evaluated);
    CHECK(outcome.image_path.empty());
    CHECK(harness.backend()->capture_call_count == 2);
    CHECK(harness.backend()->reconnect_call_count == 1);
}

TEST_CASE("CVF-006 B8 boundary: a clean inspection performs exactly one capture and no reconnect",
          "[cvf-006][B8][boundary]")
{
    SyntheticHarness harness("b8_clean", "example.pass", pass_parameters());

    const RequestHolder request("example.pass", "b8-clean", 5000u);
    const rt::InspectionOutcome outcome = harness.inspect(request.get());

    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::pass);
    CHECK(harness.backend()->capture_call_count == 1);
    CHECK(harness.backend()->reconnect_call_count == 0);
}

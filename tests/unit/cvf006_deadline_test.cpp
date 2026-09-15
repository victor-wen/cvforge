// CVF-006 independent black-box tests: end-to-end deadline behavior (brief B7).
#include <chrono>

#include "cvf006_test_support.h"

using namespace cvf006;

TEST_CASE("CVF-006 B7: a capture delay beyond timeout_ms times out with NOT_EVALUATED and a "
          "bounded elapsed time",
          "[cvf-006][B7]")
{
    SyntheticHarness harness("b7_timeout", "example.pass", pass_parameters());
    harness.backend()->inject_capture_delay(1, std::chrono::milliseconds(300));

    const RequestHolder request("example.pass", "b7-timeout", 60u);
    const auto start = std::chrono::steady_clock::now();
    const rt::InspectionOutcome outcome = harness.inspect(request.get());
    const auto wall = wall_ms_since(start);

    CHECK(outcome.status == core::Status::timeout);
    CHECK(outcome.verdict == core::Verdict::not_evaluated);
    CHECK(outcome.image_path.empty());
    CHECK(output_cleared(outcome));
    CHECK(elapsed_ms_of(outcome) >= 30);
    CHECK(elapsed_ms_of(outcome) <= 250);
    CHECK(wall >= 30);
    CHECK(wall < 400);
    CHECK(harness.backend()->capture_call_count >= 1);
}

TEST_CASE("CVF-006 B7 boundary: timeout_ms 1 expires far before an injected 300 ms capture delay",
          "[cvf-006][B7][boundary]")
{
    SyntheticHarness harness("b7_tiny", "example.pass", pass_parameters());
    harness.backend()->inject_capture_delay(1, std::chrono::milliseconds(300));

    const RequestHolder request("example.pass", "b7-tiny", 1u);
    const auto start = std::chrono::steady_clock::now();
    const rt::InspectionOutcome outcome = harness.inspect(request.get());
    const auto wall = wall_ms_since(start);

    CHECK(outcome.status == core::Status::timeout);
    CHECK(outcome.verdict == core::Verdict::not_evaluated);
    CHECK(wall < 250);
}

TEST_CASE("CVF-006 B7: timeout_ms 0 uses the 5000 ms default so a 100 ms delay still succeeds",
          "[cvf-006][B7]")
{
    SyntheticHarness harness("b7_default", "example.pass", pass_parameters());
    harness.backend()->inject_capture_delay(1, std::chrono::milliseconds(100));

    const RequestHolder request("example.pass", "b7-default", 0u);
    const auto start = std::chrono::steady_clock::now();
    const rt::InspectionOutcome outcome = harness.inspect(request.get());
    const auto wall = wall_ms_since(start);

    CHECK(outcome.status == core::Status::ok);
    CHECK(outcome.verdict == core::Verdict::pass);
    CHECK(elapsed_ms_of(outcome) >= 50);
    CHECK(wall >= 50);
    CHECK(wall < 2000);
    CHECK(harness.backend()->capture_call_count >= 1);
}

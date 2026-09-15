// CVF-006 independent black-box tests: serialized concurrent inspections (brief B12).
#include <chrono>
#include <thread>

#include "cvf006_test_support.h"

using namespace cvf006;

namespace {

struct InspectSlot {
    bool captured = false;
    rt::InspectionOutcome outcome{};
    std::int64_t wall_ms = 0;
};

}  // namespace

TEST_CASE("CVF-006 B12: two concurrent inspects both complete and are serialized", "[cvf-006][B12]")
{
    SyntheticHarness harness("b12_serialization", "example.pass", pass_parameters());
    // Both capture calls are delayed: a serialized runtime needs both delays
    // end to end, while a parallel runtime could overlap them.
    harness.backend()->inject_capture_delay(1, std::chrono::milliseconds(100));
    harness.backend()->inject_capture_delay(2, std::chrono::milliseconds(100));

    InspectSlot slots[2];
    const auto start = std::chrono::steady_clock::now();

    std::thread first([&] {
        const RequestHolder request("example.pass", "b12-first", 0u);
        const auto before = std::chrono::steady_clock::now();
        slots[0].captured =
            try_capture_outcome(harness.context()->inspect(request.get()), slots[0].outcome);
        slots[0].wall_ms = wall_ms_since(before);
    });
    std::thread second([&] {
        const RequestHolder request("example.pass", "b12-second", 0u);
        const auto before = std::chrono::steady_clock::now();
        slots[1].captured =
            try_capture_outcome(harness.context()->inspect(request.get()), slots[1].outcome);
        slots[1].wall_ms = wall_ms_since(before);
    });

    first.join();
    second.join();
    const auto total_wall_ms = wall_ms_since(start);

    for (int index = 0; index < 2; ++index) {
        INFO("inspect " << index);
        REQUIRE(slots[index].captured);
        CHECK(slots[index].outcome.status == core::Status::ok);
        CHECK(slots[index].outcome.verdict == core::Verdict::pass);
        CHECK(slots[index].wall_ms >= 50);
        CHECK_FALSE(output_cleared(slots[index].outcome));
    }

    // Serialized execution cannot finish before both 100 ms capture delays.
    CHECK(total_wall_ms >= 150);
    CHECK(total_wall_ms < 4000);
    CHECK(harness.backend()->capture_call_count == 2);
}

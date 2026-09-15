// CVF-003 independent black-box tests: dispatch containment and end-to-end
// dispatch with the example algorithm (brief B8, B9, B10).
#include <cstddef>
#include <string>
#include <utility>

#include "cvf003_test_support.h"

using namespace cvf003;

TEST_CASE("CVF-003 B8: an expired deadline fails before the algorithm is invoked",
          "[cvf-003][B8][dispatch][negative]")
{
    StubAlgorithm algorithm{"stub.deadline"};
    TestRequest request_data{white_frame(2, 2)};

    SECTION("immediate()")
    {
        request_data.deadline = core::Deadline::immediate();
    }
    SECTION("from_timeout_ms(0)")
    {
        request_data.deadline = core::Deadline::from_timeout_ms(0);
    }

    auto result = insp::dispatch(algorithm, request_data.build());

    REQUIRE_FALSE(result.has_value());
    check_failure(result.failure(), core::Status::timeout,
                  core::ErrorCode::algorithm_deadline_exceeded);
    CHECK(algorithm.inspect_calls == 0);
}

TEST_CASE("CVF-003 B8: an empty frame is rejected before the algorithm is invoked",
          "[cvf-003][B8][dispatch][negative]")
{
    StubAlgorithm algorithm{"stub.empty"};
    TestRequest request_data{empty_frame()};

    auto result = insp::dispatch(algorithm, request_data.build());

    REQUIRE_FALSE(result.has_value());
    check_failure(result.failure(), core::Status::algorithm_error,
                  core::ErrorCode::algorithm_frame_invalid);
    CHECK(algorithm.inspect_calls == 0);
}

TEST_CASE("CVF-003 B8: an algorithm exception is converted to algorithm_exception",
          "[cvf-003][B8][dispatch][negative]")
{
    StubAlgorithm algorithm{"stub.throw"};
    algorithm.throw_exception = true;
    TestRequest request_data{white_frame(2, 2)};

    auto result = insp::dispatch(algorithm, request_data.build());

    REQUIRE_FALSE(result.has_value());
    check_failure(result.failure(), core::Status::algorithm_error, core::ErrorCode::algorithm_exception);
    CHECK(algorithm.inspect_calls == 1);
}

TEST_CASE("CVF-003 B8 boundary: a result above the JSON bound fails with algorithm_output_too_large",
          "[cvf-003][B8][dispatch][boundary]")
{
    const std::size_t k_bound = static_cast<std::size_t>(insp::k_max_result_json_bytes);
    CHECK(k_bound == std::size_t{65536});

    Json measurements = Json::object();
    measurements["pad"] = std::string(66048, 'x');
    const Json defects = Json::array();
    const std::size_t serialized = measurements.dump().size() + defects.dump().size();
    REQUIRE(serialized > k_bound);

    StubAlgorithm algorithm{"stub.oversize", make_result(core::Verdict::pass, std::move(measurements),
                                                         defects, "oversize probe")};
    TestRequest request_data{white_frame(2, 2)};

    auto result = insp::dispatch(algorithm, request_data.build());

    REQUIRE_FALSE(result.has_value());
    check_failure(result.failure(), core::Status::algorithm_error,
                  core::ErrorCode::algorithm_output_too_large);
    CHECK(algorithm.inspect_calls == 1);
}

TEST_CASE("CVF-003 B8 boundary: a near-limit result below the JSON bound is returned unchanged",
          "[cvf-003][B8][dispatch][boundary]")
{
    const std::size_t k_bound = static_cast<std::size_t>(insp::k_max_result_json_bytes);

    Json measurements = Json::object();
    measurements["pad"] = std::string(64512, 'x');
    const Json defects = Json::array();
    const std::size_t serialized = measurements.dump().size() + defects.dump().size();
    REQUIRE(serialized < k_bound);
    REQUIRE(serialized > k_bound - 2048);

    StubAlgorithm algorithm{"stub.near", make_result(core::Verdict::pass, measurements, defects,
                                                     "near-limit probe")};
    TestRequest request_data{white_frame(2, 2)};

    auto result = insp::dispatch(algorithm, request_data.build());

    REQUIRE(result.has_value());
    CHECK(result.value().verdict == core::Verdict::pass);
    CHECK(result.value().measurements == measurements);
    CHECK(result.value().defects == defects);
    CHECK(result.value().diagnostics == "near-limit probe");
    CHECK(algorithm.inspect_calls == 1);
}

TEST_CASE("CVF-003 B8: a successful result is returned unchanged", "[cvf-003][B8][dispatch]")
{
    const Json measurements = Json{{"probe", 7}, {"tag", "cvf003"}};
    Json defects = Json::array();
    defects.push_back(Json::object({{"id", "d1"}}));

    StubAlgorithm algorithm{"stub.ok", make_result(core::Verdict::fail, measurements, defects,
                                                   "cvf003-diagnostics")};
    TestRequest request_data{white_frame(2, 2)};

    auto result = insp::dispatch(algorithm, request_data.build());

    REQUIRE(result.has_value());
    CHECK(result.value().verdict == core::Verdict::fail);
    CHECK(result.value().measurements == measurements);
    CHECK(result.value().defects == defects);
    CHECK(result.value().diagnostics == "cvf003-diagnostics");
    CHECK(algorithm.inspect_calls == 1);
}

TEST_CASE("CVF-003 B9: dispatch end-to-end returns OK + PASS on a white frame consistent with the "
          "threshold",
          "[cvf-003][B9][dispatch][example]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{white_frame(4, 3)};
    request_data.parameters = Json{{"threshold", 200}};

    auto result = insp::dispatch(example, request_data.build());

    REQUIRE(result.has_value());
    CHECK(result.value().verdict == core::Verdict::pass);
    CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(1.0));
}

TEST_CASE("CVF-003 B9: dispatch end-to-end returns OK + FAIL on a black frame (technical OK, "
          "product FAIL)",
          "[cvf-003][B9][dispatch][example]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{black_frame(4, 3)};

    auto result = insp::dispatch(example, request_data.build());

    REQUIRE(result.has_value());
    CHECK(result.value().verdict == core::Verdict::fail);
    CHECK(result.value().verdict != core::Verdict::not_evaluated);
    CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(0.0));
}

TEST_CASE("CVF-003 B9: the registered example algorithm dispatches through find()",
          "[cvf-003][B9][dispatch][example]")
{
    insp::AlgorithmRegistry registry;
    REQUIRE(alg::register_compiled_algorithms(registry).has_value());
    const auto* found = registry.find("example.threshold").value();
    REQUIRE(found != nullptr);

    TestRequest request_data{white_frame(2, 2)};
    request_data.parameters = Json{{"threshold", 200}};

    auto result = insp::dispatch(*found, request_data.build());

    REQUIRE(result.has_value());
    CHECK(result.value().verdict == core::Verdict::pass);
}

TEST_CASE("CVF-003 B10: 1x1 uniform frames yield exact outcomes",
          "[cvf-003][B10][dispatch][example][boundary]")
{
    alg::ExampleThresholdAlgorithm example;

    SECTION("1x1 white -> white_pixels 1, pass_ratio 1.0, PASS")
    {
        TestRequest request_data{white_frame(1, 1)};

        auto result = insp::dispatch(example, request_data.build());

        REQUIRE(result.has_value());
        CHECK(require_field(result.value().measurements, "white_pixels") == 1);
        CHECK(require_field(result.value().measurements, "total_pixels") == 1);
        CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(1.0));
        CHECK(result.value().verdict == core::Verdict::pass);
    }

    SECTION("1x1 black -> white_pixels 0, pass_ratio 0.0, FAIL")
    {
        TestRequest request_data{black_frame(1, 1)};

        auto result = insp::dispatch(example, request_data.build());

        REQUIRE(result.has_value());
        CHECK(require_field(result.value().measurements, "white_pixels") == 0);
        CHECK(require_field(result.value().measurements, "total_pixels") == 1);
        CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(0.0));
        CHECK(result.value().verdict == core::Verdict::fail);
    }
}

TEST_CASE("CVF-003 B10 boundary: pass_ratio exactly equal to min_pass_ratio yields PASS",
          "[cvf-003][B10][dispatch][example][boundary]")
{
    alg::ExampleThresholdAlgorithm example;

    SECTION("half-white 2x1 frame with min_pass_ratio 0.5 -> PASS")
    {
        TestRequest request_data{half_white_frame()};
        request_data.parameters = Json{{"min_pass_ratio", 0.5}};

        auto result = insp::dispatch(example, request_data.build());

        REQUIRE(result.has_value());
        CHECK(require_field(result.value().measurements, "white_pixels") == 1);
        CHECK(require_field(result.value().measurements, "total_pixels") == 2);
        CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(0.5));
        CHECK(result.value().verdict == core::Verdict::pass);
    }

    SECTION("the same frame with min_pass_ratio 0.6 -> FAIL (control)")
    {
        TestRequest request_data{half_white_frame()};
        request_data.parameters = Json{{"min_pass_ratio", 0.6}};

        auto result = insp::dispatch(example, request_data.build());

        REQUIRE(result.has_value());
        CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(0.5));
        CHECK(result.value().verdict == core::Verdict::fail);
    }

    SECTION("all-black frame with min_pass_ratio 0.0 -> PASS (0.0 >= 0.0)")
    {
        TestRequest request_data{black_frame(2, 2)};
        request_data.parameters = Json{{"min_pass_ratio", 0.0}};

        auto result = insp::dispatch(example, request_data.build());

        REQUIRE(result.has_value());
        CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(0.0));
        CHECK(result.value().verdict == core::Verdict::pass);
    }

    SECTION("all-white frame with min_pass_ratio 1.0 -> PASS (1.0 >= 1.0)")
    {
        TestRequest request_data{white_frame(2, 2)};
        request_data.parameters = Json{{"min_pass_ratio", 1.0}};

        auto result = insp::dispatch(example, request_data.build());

        REQUIRE(result.has_value());
        CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(1.0));
        CHECK(result.value().verdict == core::Verdict::pass);
    }
}

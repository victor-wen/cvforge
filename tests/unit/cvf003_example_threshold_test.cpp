// CVF-003 independent black-box tests: ExampleThresholdAlgorithm parameter
// validation, input_json handling, deadline honoring, and deterministic inspect
// outcomes (brief B3-B7).
#include <optional>
#include <string>
#include <vector>

#include "cvf003_test_support.h"

using namespace cvf003;

TEST_CASE("CVF-003 B3: default and explicit valid parameters are accepted",
          "[cvf-003][B3][example]")
{
    const alg::ExampleThresholdAlgorithm example;
    const std::vector<Json> valid_parameters = {
        Json::object(),
        Json{{"threshold", 128}},
        Json{{"min_pass_ratio", 0.5}},
        Json{{"threshold", 128}, {"min_pass_ratio", 0.5}},
    };

    for (const auto& parameters : valid_parameters) {
        INFO("parameters: " << parameters.dump());
        CHECK(example.validate_parameters(parameters).has_value());
    }
}

TEST_CASE("CVF-003 B3 boundary: threshold 0/255 and min_pass_ratio 0.0/1.0 are accepted",
          "[cvf-003][B3][example][boundary]")
{
    const alg::ExampleThresholdAlgorithm example;
    const std::vector<Json> boundary_parameters = {
        Json{{"threshold", 0}},
        Json{{"threshold", 255}},
        Json{{"min_pass_ratio", 0.0}},
        Json{{"min_pass_ratio", 1.0}},
    };

    for (const auto& parameters : boundary_parameters) {
        INFO("parameters: " << parameters.dump());
        CHECK(example.validate_parameters(parameters).has_value());
    }
}

TEST_CASE("CVF-003 B3 negative: an unknown parameter key is rejected",
          "[cvf-003][B3][example][negative]")
{
    const alg::ExampleThresholdAlgorithm example;

    auto validated = example.validate_parameters(Json{{"bogus", 1}});

    REQUIRE_FALSE(validated.has_value());
    check_failure(validated.failure(), core::Status::config_error,
                  core::ErrorCode::algorithm_parameters_invalid);
}

TEST_CASE("CVF-003 B3 negative: wrong parameter types are rejected",
          "[cvf-003][B3][example][negative]")
{
    const alg::ExampleThresholdAlgorithm example;
    const std::vector<Json> invalid_parameters = {
        Json{{"threshold", 12.5}},
        Json{{"threshold", "128"}},
        Json{{"min_pass_ratio", "0.5"}},
    };

    for (const auto& parameters : invalid_parameters) {
        INFO("parameters: " << parameters.dump());
        auto validated = example.validate_parameters(parameters);
        REQUIRE_FALSE(validated.has_value());
        check_failure(validated.failure(), core::Status::config_error,
                      core::ErrorCode::algorithm_parameters_invalid);
    }
}

TEST_CASE("CVF-003 B3 negative: out-of-range parameter values are rejected",
          "[cvf-003][B3][example][negative]")
{
    const alg::ExampleThresholdAlgorithm example;
    const std::vector<Json> invalid_parameters = {
        Json{{"threshold", -1}},
        Json{{"threshold", 256}},
        Json{{"min_pass_ratio", -0.01}},
        Json{{"min_pass_ratio", 1.01}},
    };

    for (const auto& parameters : invalid_parameters) {
        INFO("parameters: " << parameters.dump());
        auto validated = example.validate_parameters(parameters);
        REQUIRE_FALSE(validated.has_value());
        check_failure(validated.failure(), core::Status::config_error,
                      core::ErrorCode::algorithm_parameters_invalid);
    }
}

TEST_CASE("CVF-003 B4: an all-white frame reports exact full-pass measurements and PASS",
          "[cvf-003][B4][example]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{white_frame(4, 3)};

    auto result = example.inspect(request_data.build());

    REQUIRE(result.has_value());
    const auto& outcome = result.value();
    CHECK(outcome.verdict == core::Verdict::pass);
    CHECK(outcome.verdict != core::Verdict::not_evaluated);
    CHECK(require_field(outcome.measurements, "white_pixels") == 12);
    CHECK(require_field(outcome.measurements, "total_pixels") == 12);
    CHECK(numeric_field(outcome.measurements, "pass_ratio") == Catch::Approx(1.0));
    REQUIRE(outcome.defects.is_array());
    CHECK(outcome.defects.empty());
    CHECK_FALSE(outcome.diagnostics.empty());
}

TEST_CASE("CVF-003 B4: an all-black frame reports exact zero measurements and FAIL",
          "[cvf-003][B4][example]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{black_frame(4, 3)};

    auto result = example.inspect(request_data.build());

    REQUIRE(result.has_value());
    const auto& outcome = result.value();
    CHECK(outcome.verdict == core::Verdict::fail);
    CHECK(outcome.verdict != core::Verdict::not_evaluated);
    CHECK(require_field(outcome.measurements, "white_pixels") == 0);
    CHECK(require_field(outcome.measurements, "total_pixels") == 12);
    CHECK(numeric_field(outcome.measurements, "pass_ratio") == Catch::Approx(0.0));
    REQUIRE(outcome.defects.is_array());
    CHECK(outcome.defects.empty());
    CHECK_FALSE(outcome.diagnostics.empty());
}

TEST_CASE("CVF-003 B5: invert flips an all-white frame to zero white pixels and FAIL",
          "[cvf-003][B5][example]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{white_frame(2, 2)};
    request_data.input_json = Json{{"invert", true}};

    auto result = example.inspect(request_data.build());

    REQUIRE(result.has_value());
    CHECK(require_field(result.value().measurements, "white_pixels") == 0);
    CHECK(require_field(result.value().measurements, "total_pixels") == 4);
    CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(0.0));
    CHECK(result.value().verdict == core::Verdict::fail);
}

TEST_CASE("CVF-003 B5: invert flips an all-black frame to full white pixels and PASS with "
          "min_pass_ratio 0.0",
          "[cvf-003][B5][example]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{black_frame(2, 2)};
    request_data.parameters = Json{{"min_pass_ratio", 0.0}};
    request_data.input_json = Json{{"invert", true}};

    auto result = example.inspect(request_data.build());

    REQUIRE(result.has_value());
    CHECK(require_field(result.value().measurements, "white_pixels") == 4);
    CHECK(require_field(result.value().measurements, "total_pixels") == 4);
    CHECK(numeric_field(result.value().measurements, "pass_ratio") == Catch::Approx(1.0));
    CHECK(result.value().verdict == core::Verdict::pass);
}

TEST_CASE("CVF-003 B6: absent input_json is accepted", "[cvf-003][B6][example]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{white_frame(1, 1)};

    auto result = example.inspect(request_data.build());

    REQUIRE(result.has_value());
    CHECK(result.value().verdict == core::Verdict::pass);
}

TEST_CASE("CVF-003 B6 negative: a non-object input_json is rejected",
          "[cvf-003][B6][example][negative]")
{
    alg::ExampleThresholdAlgorithm example;
    const std::vector<Json> invalid_inputs = {
        Json::array(),
        Json(7),
        Json("probe"),
    };

    for (const auto& input : invalid_inputs) {
        INFO("input_json: " << input.dump());
        TestRequest request_data{white_frame(1, 1)};
        request_data.input_json = input;

        auto result = example.inspect(request_data.build());

        REQUIRE_FALSE(result.has_value());
        check_failure(result.failure(), core::Status::invalid_argument,
                      core::ErrorCode::algorithm_input_invalid);
    }
}

TEST_CASE("CVF-003 B6 negative: an unsupported input_json key is rejected",
          "[cvf-003][B6][example][negative]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{white_frame(1, 1)};
    request_data.input_json = Json{{"bogus", true}};

    auto result = example.inspect(request_data.build());

    REQUIRE_FALSE(result.has_value());
    check_failure(result.failure(), core::Status::invalid_argument,
                  core::ErrorCode::algorithm_input_invalid);
}

TEST_CASE("CVF-003 B6 negative: a non-boolean invert is rejected",
          "[cvf-003][B6][example][negative]")
{
    alg::ExampleThresholdAlgorithm example;
    const std::vector<Json> invalid_inputs = {
        Json{{"invert", 1}},
        Json{{"invert", "true"}},
    };

    for (const auto& input : invalid_inputs) {
        INFO("input_json: " << input.dump());
        TestRequest request_data{white_frame(1, 1)};
        request_data.input_json = input;

        auto result = example.inspect(request_data.build());

        REQUIRE_FALSE(result.has_value());
        check_failure(result.failure(), core::Status::invalid_argument,
                      core::ErrorCode::algorithm_input_invalid);
    }
}

TEST_CASE("CVF-003 B6: a boolean invert value is accepted", "[cvf-003][B6][example]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{white_frame(1, 1)};
    request_data.input_json = Json{{"invert", false}};

    auto result = example.inspect(request_data.build());

    REQUIRE(result.has_value());
    CHECK(result.value().verdict == core::Verdict::pass);
    CHECK(require_field(result.value().measurements, "white_pixels") == 1);
}

TEST_CASE("CVF-003 B7: an expired deadline yields timeout/algorithm_deadline_exceeded",
          "[cvf-003][B7][example][negative]")
{
    alg::ExampleThresholdAlgorithm example;
    TestRequest request_data{white_frame(2, 2)};

    SECTION("immediate()")
    {
        request_data.deadline = core::Deadline::immediate();
    }
    SECTION("from_timeout_ms(0)")
    {
        request_data.deadline = core::Deadline::from_timeout_ms(0);
    }

    auto result = example.inspect(request_data.build());

    REQUIRE_FALSE(result.has_value());
    check_failure(result.failure(), core::Status::timeout,
                  core::ErrorCode::algorithm_deadline_exceeded);
}

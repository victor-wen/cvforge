/*
 * Developer regression tests for the CVF-003 inspection contract.
 *
 * These cover the frozen boundary rules the implementation adds around the
 * independent cvf003_* suite: algorithm failure propagation through dispatch,
 * the exact JSON bound, key validation beyond the brief's samples, and the
 * strict cv2-style semantics of the example algorithm.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include "algorithms/example_threshold.h"
#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"
#include "inspection/algorithm.h"
#include "inspection/registry.h"

namespace {

using Json = nlohmann::json;
namespace core = cvforwin::core;
namespace insp = cvforwin::inspection;
namespace alg = cvforwin::algorithms;
namespace cam = cvforwin::camera;

cam::CapturedFrame uniform_frame(int width, int height, int bgr_value)
{
    cam::CapturedFrame frame{};
    frame.pixels = cv::Mat(height, width, CV_8UC3, cv::Scalar(bgr_value, bgr_value, bgr_value));
    frame.metadata.width = static_cast<std::uint32_t>(width);
    frame.metadata.height = static_cast<std::uint32_t>(height);
    frame.metadata.pixel_format = cam::PixelFormat::bgr8;
    return frame;
}

cam::CapturedFrame gray_frame(std::uint8_t value)
{
    cam::CapturedFrame frame{};
    frame.pixels = cv::Mat(1, 1, CV_8UC1, cv::Scalar(value));
    frame.metadata.width = 1;
    frame.metadata.height = 1;
    frame.metadata.pixel_format = cam::PixelFormat::mono8;
    return frame;
}

struct RequestHolder {
    cam::CapturedFrame frame;
    Json parameters = Json::object();
    std::optional<Json> input_json = std::nullopt;
    core::Deadline deadline = core::Deadline::from_timeout_ms(5000);

    insp::AlgorithmRequest build() const
    {
        return insp::AlgorithmRequest{
            .frame = frame,
            .parameters = parameters,
            .input_json = input_json,
            .deadline = deadline,
        };
    }
};

class FixedResultAlgorithm final : public insp::IInspectionAlgorithm {
public:
    FixedResultAlgorithm(std::string key, insp::AlgorithmResult result)
        : key_(std::move(key)), result_(std::move(result))
    {}

    std::string_view key() const noexcept override
    {
        return key_;
    }

    core::Result<void> validate_parameters(const Json&) const override
    {
        return core::Result<void>{};
    }

    core::Result<insp::AlgorithmResult> inspect(const insp::AlgorithmRequest&) override
    {
        return result_;
    }

private:
    std::string key_;
    insp::AlgorithmResult result_;
};

class FailingAlgorithm final : public insp::IInspectionAlgorithm {
public:
    std::string_view key() const noexcept override
    {
        return "test.failing";
    }

    core::Result<void> validate_parameters(const Json&) const override
    {
        return core::Result<void>{};
    }

    core::Result<insp::AlgorithmResult> inspect(const insp::AlgorithmRequest&) override
    {
        return core::make_failure(core::Status::config_error, core::ErrorCode::algorithm_parameters_invalid,
                                  "test.failing rejected its parameters");
    }
};

}  // namespace

TEST_CASE("dispatch propagates an algorithm failure unchanged", "[inspection][dispatch]")
{
    FailingAlgorithm algorithm;
    RequestHolder request{uniform_frame(2, 2, 255)};

    auto result = insp::dispatch(algorithm, request.build());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.failure().status == core::Status::config_error);
    CHECK(result.failure().code == core::ErrorCode::algorithm_parameters_invalid);
    CHECK(result.failure().message == "test.failing rejected its parameters");
}

TEST_CASE("dispatch accepts a result exactly at the JSON bound", "[inspection][dispatch][boundary]")
{
    const Json defects = Json::array();
    Json measurements = Json::object();
    measurements["pad"] = std::string(1, 'x');
    const std::size_t overhead = measurements.dump().size() - 1u;
    const std::size_t padding = insp::k_max_result_json_bytes - defects.dump().size() - overhead;
    measurements["pad"] = std::string(padding, 'x');
    REQUIRE(measurements.dump().size() + defects.dump().size() == insp::k_max_result_json_bytes);

    FixedResultAlgorithm algorithm{"test.exact_bound",
                                   insp::AlgorithmResult{.verdict = core::Verdict::pass,
                                                         .measurements = std::move(measurements),
                                                         .defects = defects,
                                                         .diagnostics = "exact bound"}};
    RequestHolder request{uniform_frame(2, 2, 255)};

    auto result = insp::dispatch(algorithm, request.build());

    REQUIRE(result.has_value());
    CHECK(result.value().verdict == core::Verdict::pass);
}

TEST_CASE("registry rejects keys with characters outside [a-z0-9._-]", "[inspection][registry][negative]")
{
    insp::AlgorithmRegistry registry;
    const std::string digits_only = "0123456789";
    REQUIRE(registry.add(std::make_unique<FixedResultAlgorithm>(digits_only, insp::AlgorithmResult{})).has_value());

    for (const std::string& invalid : {std::string("key/slash"), std::string("key:colon"), std::string("key+plus")}) {
        auto added = registry.add(std::make_unique<FixedResultAlgorithm>(invalid, insp::AlgorithmResult{}));
        REQUIRE_FALSE(added.has_value());
        CHECK(added.failure().status == core::Status::invalid_argument);
        CHECK(added.failure().code == core::ErrorCode::algorithm_key_invalid);
    }
    CHECK(registry.size() == 1u);
    CHECK(registry.find(digits_only).has_value());
}

TEST_CASE("registry find treats a malformed key as an unknown key", "[inspection][registry][negative]")
{
    insp::AlgorithmRegistry registry;
    REQUIRE(registry.add(std::make_unique<FixedResultAlgorithm>("known.key", insp::AlgorithmResult{})).has_value());

    auto found = registry.find("Not/AKey");

    REQUIRE_FALSE(found.has_value());
    CHECK(found.failure().status == core::Status::algorithm_error);
    CHECK(found.failure().code == core::ErrorCode::algorithm_not_found);
}

TEST_CASE("example.threshold rejects non-object parameters", "[inspection][example][negative]")
{
    const alg::ExampleThresholdAlgorithm example;
    for (const Json& parameters : {Json::array(), Json(7), Json("probe"), Json(nullptr)}) {
        auto validated = example.validate_parameters(parameters);
        REQUIRE_FALSE(validated.has_value());
        CHECK(validated.failure().status == core::Status::config_error);
        CHECK(validated.failure().code == core::ErrorCode::algorithm_parameters_invalid);
    }
}

TEST_CASE("example.threshold uses a strict gray > threshold comparison", "[inspection][example][boundary]")
{
    alg::ExampleThresholdAlgorithm example;

    SECTION("a pixel exactly at the threshold is not white")
    {
        RequestHolder request{gray_frame(100)};
        request.parameters = Json{{"threshold", 100}};

        auto result = example.inspect(request.build());

        REQUIRE(result.has_value());
        CHECK(result.value().measurements.at("white_pixels").get<std::int64_t>() == 0);
        CHECK(result.value().measurements.at("total_pixels").get<std::int64_t>() == 1);
        CHECK(result.value().measurements.at("pass_ratio").get<double>() == 0.0);
        CHECK(result.value().verdict == core::Verdict::fail);
    }

    SECTION("a pixel one step above the threshold is white")
    {
        RequestHolder request{gray_frame(101)};
        request.parameters = Json{{"threshold", 100}};

        auto result = example.inspect(request.build());

        REQUIRE(result.has_value());
        CHECK(result.value().measurements.at("white_pixels").get<std::int64_t>() == 1);
        CHECK(result.value().verdict == core::Verdict::pass);
    }
}

TEST_CASE("example.threshold accepts integer min_pass_ratio bounds", "[inspection][example][boundary]")
{
    const alg::ExampleThresholdAlgorithm example;

    CHECK(example.validate_parameters(Json{{"min_pass_ratio", 0}}).has_value());
    CHECK(example.validate_parameters(Json{{"min_pass_ratio", 1}}).has_value());
    CHECK_FALSE(example.validate_parameters(Json{{"threshold", true}}).has_value());
    CHECK_FALSE(example.validate_parameters(Json{{"min_pass_ratio", false}}).has_value());
}

TEST_CASE("example.threshold rejects an empty frame in direct inspect", "[inspection][example][negative]")
{
    alg::ExampleThresholdAlgorithm example;
    RequestHolder request{cam::CapturedFrame{}};

    auto result = example.inspect(request.build());

    REQUIRE_FALSE(result.has_value());
    CHECK(result.failure().status == core::Status::algorithm_error);
    CHECK(result.failure().code == core::ErrorCode::algorithm_frame_invalid);
}

TEST_CASE("example.threshold diagnostics are non-empty and bounded", "[inspection][example]")
{
    alg::ExampleThresholdAlgorithm example;
    RequestHolder request{uniform_frame(4, 3, 255)};
    request.input_json = Json{{"invert", true}};

    auto result = insp::dispatch(example, request.build());

    REQUIRE(result.has_value());
    CHECK_FALSE(result.value().diagnostics.empty());
    CHECK(result.value().diagnostics.size() < 256u);
    CHECK(result.value().measurements.is_object());
    CHECK(result.value().defects.is_array());
}

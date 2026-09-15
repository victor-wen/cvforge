#pragma once

// CVF-003 independent black-box test support (owner: test-engineer).
//
// Shared aliases, deterministic run-time frame builders, and the
// IInspectionAlgorithm test double used by the independent inspection-contract
// suite. This header compiles only against the frozen interface headers listed
// in the CVF-003 test brief; it never includes production .cpp files and never
// inspects implementation state.
//
// Frozen interface headers first: a missing interface header must be the first
// diagnostic in the author (RED) phase.

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"

#include "camera/captured_frame.h"

#include "inspection/algorithm.h"
#include "inspection/registry.h"

#include "algorithms/compiled_algorithms.h"
#include "algorithms/example_threshold.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cvf003 {

namespace core = cvforwin::core;
namespace insp = cvforwin::inspection;
namespace alg = cvforwin::algorithms;
namespace cam = cvforwin::camera;

using Json = nlohmann::json;

// --- deterministic run-time frames -----------------------------------------

inline cam::CapturedFrame uniform_frame(int width, int height, int bgr_value)
{
    cam::CapturedFrame frame{};
    frame.pixels = cv::Mat(height, width, CV_8UC3, cv::Scalar(bgr_value, bgr_value, bgr_value));
    frame.metadata.width = width;
    frame.metadata.height = height;
    frame.metadata.pixel_format = cam::PixelFormat::bgr8;
    return frame;
}

inline cam::CapturedFrame white_frame(int width, int height)
{
    return uniform_frame(width, height, 255);
}

inline cam::CapturedFrame black_frame(int width, int height)
{
    return uniform_frame(width, height, 0);
}

// 2x1 BGR8 frame: pixel x=0 is white, pixel x=1 is black, so the pass ratio is
// exactly 0.5 when the white pixel counts.
inline cam::CapturedFrame half_white_frame()
{
    cam::CapturedFrame frame{};
    frame.pixels = cv::Mat(1, 2, CV_8UC3, cv::Scalar(0, 0, 0));
    frame.pixels.at<cv::Vec3b>(0, 0) = cv::Vec3b(255, 255, 255);
    frame.metadata.width = 2;
    frame.metadata.height = 1;
    frame.metadata.pixel_format = cam::PixelFormat::bgr8;
    return frame;
}

inline cam::CapturedFrame empty_frame()
{
    return cam::CapturedFrame{};
}

// --- request holder ---------------------------------------------------------

// Owns the AlgorithmRequest inputs so the request's reference members stay
// valid for the duration of the test (a temporary would dangle).
struct TestRequest {
    explicit TestRequest(cam::CapturedFrame frame_in)
        : frame(std::move(frame_in))
    {}

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

// --- JSON helpers -----------------------------------------------------------

inline const Json& require_field(const Json& object, const char* key)
{
    REQUIRE(object.is_object());
    const auto it = object.find(key);
    REQUIRE(it != object.end());
    return *it;
}

inline double numeric_field(const Json& object, const char* key)
{
    const Json& field = require_field(object, key);
    REQUIRE(field.is_number());
    return field.get<double>();
}

// --- result helpers ---------------------------------------------------------

inline insp::AlgorithmResult make_result(core::Verdict verdict, Json measurements, Json defects,
                                         std::string diagnostics)
{
    return insp::AlgorithmResult{
        .verdict = verdict,
        .measurements = std::move(measurements),
        .defects = std::move(defects),
        .diagnostics = std::move(diagnostics),
    };
}

inline void check_failure(const core::Failure& failure, core::Status status, core::ErrorCode code)
{
    CHECK(failure.status == status);
    CHECK(failure.code == code);
}

// --- test double ------------------------------------------------------------

// Deterministic IInspectionAlgorithm double: counts invocations and returns one
// pre-programmed result, optionally throwing from inspect.
class StubAlgorithm final : public insp::IInspectionAlgorithm {
public:
    explicit StubAlgorithm(std::string key)
        : key_(std::move(key)),
          result_(make_result(core::Verdict::pass, Json::object(), Json::array(),
                              "cvf003 stub diagnostics"))
    {}

    StubAlgorithm(std::string key, insp::AlgorithmResult result)
        : key_(std::move(key)), result_(std::move(result))
    {}

    std::string_view key() const noexcept override
    {
        return key_;
    }

    core::Result<void> validate_parameters(const Json&) const override
    {
        ++validate_calls;
        return core::Result<void>{};
    }

    core::Result<insp::AlgorithmResult> inspect(const insp::AlgorithmRequest&) override
    {
        ++inspect_calls;
        if (throw_exception) {
            throw std::runtime_error("cvf003 stub algorithm probe exception");
        }
        return core::Result<insp::AlgorithmResult>{result_};
    }

    mutable int validate_calls = 0;
    int inspect_calls = 0;
    bool throw_exception = false;

private:
    std::string key_;
    insp::AlgorithmResult result_;
};

}  // namespace cvf003

#include "algorithms/example_threshold.h"

#include <cstdint>
#include <string>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "core/error.h"

namespace cvforwin::algorithms {

namespace {

constexpr int k_min_threshold = 0;
constexpr int k_max_threshold = 255;
constexpr double k_min_pass_ratio = 0.0;
constexpr double k_max_pass_ratio = 1.0;

struct ThresholdParameters {
    int threshold = ExampleThresholdAlgorithm::k_default_threshold;
    double min_pass_ratio = ExampleThresholdAlgorithm::k_default_min_pass_ratio;
};

core::Failure parameters_invalid(std::string message)
{
    return core::make_failure(core::Status::config_error, core::ErrorCode::algorithm_parameters_invalid,
                              std::move(message));
}

core::Failure input_invalid(std::string message)
{
    return core::make_failure(core::Status::invalid_argument, core::ErrorCode::algorithm_input_invalid,
                              std::move(message));
}

core::Failure frame_invalid()
{
    return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_frame_invalid,
                              "example.threshold requires a non-empty BGR8 or mono8 frame");
}

core::Result<ThresholdParameters> parse_parameters(const nlohmann::json& parameters)
{
    if (!parameters.is_object()) {
        return parameters_invalid("threshold parameters must be a JSON object");
    }

    ThresholdParameters parsed;
    for (const auto& [key, value] : parameters.items()) {
        if (key == "threshold") {
            if (!value.is_number_integer()) {
                return parameters_invalid("threshold must be an integer");
            }
            const auto threshold = value.get<std::int64_t>();
            if (threshold < k_min_threshold || threshold > k_max_threshold) {
                return parameters_invalid("threshold must be within 0..255");
            }
            parsed.threshold = static_cast<int>(threshold);
        } else if (key == "min_pass_ratio") {
            if (!value.is_number()) {
                return parameters_invalid("min_pass_ratio must be a number");
            }
            const double ratio = value.get<double>();
            if (ratio < k_min_pass_ratio || ratio > k_max_pass_ratio) {
                return parameters_invalid("min_pass_ratio must be within 0.0..1.0");
            }
            parsed.min_pass_ratio = ratio;
        } else {
            return parameters_invalid("unknown threshold parameter: " + key);
        }
    }
    return parsed;
}

core::Result<bool> parse_invert(const std::optional<nlohmann::json>& input_json)
{
    if (!input_json.has_value()) {
        return false;
    }

    const nlohmann::json& input = *input_json;
    if (!input.is_object()) {
        return input_invalid("input_json must be a JSON object");
    }

    bool invert = false;
    for (const auto& [key, value] : input.items()) {
        if (key == "invert") {
            if (!value.is_boolean()) {
                return input_invalid("invert must be a boolean");
            }
            invert = value.get<bool>();
        } else {
            return input_invalid("unknown input_json key: " + key);
        }
    }
    return invert;
}

}  // namespace

std::string_view ExampleThresholdAlgorithm::key() const noexcept
{
    return k_key;
}

core::Result<void> ExampleThresholdAlgorithm::validate_parameters(const nlohmann::json& parameters) const
{
    auto parsed = parse_parameters(parameters);
    if (!parsed.has_value()) {
        return parsed.failure();
    }
    return core::Result<void>{};
}

core::Result<inspection::AlgorithmResult> ExampleThresholdAlgorithm::inspect(
    const inspection::AlgorithmRequest& request)
{
    if (request.deadline.expired()) {
        return core::make_failure(core::Status::timeout, core::ErrorCode::algorithm_deadline_exceeded,
                                  "example.threshold deadline expired before image processing");
    }

    auto parsed = parse_parameters(request.parameters);
    if (!parsed.has_value()) {
        return parsed.failure();
    }

    auto invert = parse_invert(request.input_json);
    if (!invert.has_value()) {
        return invert.failure();
    }

    const cv::Mat& pixels = request.frame.pixels;
    if (pixels.empty()) {
        return frame_invalid();
    }

    cv::Mat gray;
    if (pixels.type() == CV_8UC3) {
        cv::cvtColor(pixels, gray, cv::COLOR_BGR2GRAY);
    } else if (pixels.type() == CV_8UC1) {
        gray = pixels;
    } else {
        return frame_invalid();
    }

    cv::Mat mask;
    cv::threshold(gray, mask, static_cast<double>(parsed.value().threshold), 255.0, cv::THRESH_BINARY);
    if (invert.value()) {
        cv::bitwise_not(mask, mask);
    }

    const int white_pixels = cv::countNonZero(mask);
    const auto total_pixels = mask.total();
    const double pass_ratio =
        total_pixels == 0 ? 0.0 : static_cast<double>(white_pixels) / static_cast<double>(total_pixels);

    inspection::AlgorithmResult result;
    result.verdict = pass_ratio >= parsed.value().min_pass_ratio ? core::Verdict::pass : core::Verdict::fail;
    result.measurements = nlohmann::json{
        {"white_pixels", static_cast<std::int64_t>(white_pixels)},
        {"total_pixels", static_cast<std::int64_t>(total_pixels)},
        {"pass_ratio", pass_ratio},
    };
    result.defects = nlohmann::json::array();
    result.diagnostics = "example.threshold white_pixels=" + std::to_string(white_pixels) +
                         " total_pixels=" + std::to_string(total_pixels) +
                         " pass_ratio=" + std::to_string(pass_ratio) +
                         " threshold=" + std::to_string(parsed.value().threshold);
    return result;
}

}  // namespace cvforwin::algorithms

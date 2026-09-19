#include "algorithms/template_match.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "core/error.h"

namespace cvforwin::algorithms {

namespace {

/*
 * The reference algorithm bounds its search window so a recipe cannot request
 * an unbounded match. The configured capture frame is checked against the
 * decoded frame at inspect time; preparation validates the requested window
 * against this compiled extent.
 */
constexpr int k_max_roi_extent = 64;
constexpr int k_max_decoded_dimension = 8192;
constexpr std::size_t k_max_decoded_bytes = 134217728u;
constexpr std::string_view k_method_ccoeff_normed = "ccoeff_normed";

struct TemplateParameters {
    std::string template_asset;
    int roi_x = 0;
    int roi_y = 0;
    int roi_width = 0;
    int roi_height = 0;
    double threshold = 0.0;
};

core::Failure parameters_invalid(std::string message)
{
    return core::make_failure(core::Status::config_error, core::ErrorCode::algorithm_parameters_invalid,
                              std::move(message));
}

core::Failure frame_invalid(std::string message)
{
    return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_frame_invalid,
                              std::move(message));
}

bool is_logical_key(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 64u) {
        return false;
    }
    for (const char character : value) {
        const bool valid = (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '.' ||
                           character == '_' || character == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

core::Result<TemplateParameters> parse_parameters(const nlohmann::json& parameters)
{
    if (!parameters.is_object()) {
        return parameters_invalid("template.match parameters must be a JSON object");
    }

    TemplateParameters parsed;
    bool has_asset = false;
    bool has_roi = false;
    bool has_method = false;
    bool has_threshold = false;

    for (const auto& [key, value] : parameters.items()) {
        if (key == "template_asset") {
            if (!value.is_string()) {
                return parameters_invalid("template_asset must be a string");
            }
            parsed.template_asset = value.get<std::string>();
            if (!is_logical_key(parsed.template_asset)) {
                return parameters_invalid("template_asset must match [a-z0-9._-]{1,64}");
            }
            has_asset = true;
        } else if (key == "roi") {
            if (!value.is_array() || value.size() != 4u) {
                return parameters_invalid("roi must be [x, y, width, height]");
            }
            int values[4] = {0, 0, 0, 0};
            for (std::size_t index = 0; index < 4u; ++index) {
                if (!value.at(index).is_number_integer()) {
                    return parameters_invalid("roi entries must be integers");
                }
                values[index] = value.at(index).get<int>();
            }
            parsed.roi_x = values[0];
            parsed.roi_y = values[1];
            parsed.roi_width = values[2];
            parsed.roi_height = values[3];
            has_roi = true;
        } else if (key == "method") {
            if (!value.is_string() || value.get<std::string>() != k_method_ccoeff_normed) {
                return parameters_invalid("method must be \"ccoeff_normed\"");
            }
            has_method = true;
        } else if (key == "threshold") {
            if (!value.is_number()) {
                return parameters_invalid("threshold must be a number");
            }
            parsed.threshold = value.get<double>();
            has_threshold = true;
        } else {
            return parameters_invalid("unknown template.match parameter: " + key);
        }
    }

    if (!has_asset || !has_roi || !has_method || !has_threshold) {
        return parameters_invalid(
            "template.match requires template_asset, roi, method, and threshold");
    }
    if (!std::isfinite(parsed.threshold) || parsed.threshold < 0.0 || parsed.threshold > 1.0) {
        return parameters_invalid("threshold must be a finite number in 0.0..1.0");
    }
    if (parsed.roi_x < 0 || parsed.roi_y < 0) {
        return parameters_invalid("roi origin must be non-negative");
    }
    if (parsed.roi_width < 1 || parsed.roi_height < 1 || parsed.roi_width > k_max_roi_extent ||
        parsed.roi_height > k_max_roi_extent) {
        return parameters_invalid("roi dimensions must be within 1..64");
    }
    return parsed;
}

core::Result<cv::Mat> decode_grayscale(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return parameters_invalid("template asset is empty");
    }
    const cv::Mat encoded(1, static_cast<int>(bytes.size()), CV_8UC1,
                          const_cast<std::uint8_t*>(bytes.data()));
    cv::Mat decoded = cv::imdecode(encoded, cv::IMREAD_GRAYSCALE);
    if (decoded.empty()) {
        return parameters_invalid("template asset is not a decodable image");
    }
    if (decoded.cols > k_max_decoded_dimension || decoded.rows > k_max_decoded_dimension) {
        return parameters_invalid("template image exceeds the 8192x8192 pixel bound");
    }
    const std::size_t decoded_bytes = decoded.total() * decoded.elemSize();
    if (decoded_bytes > k_max_decoded_bytes) {
        return parameters_invalid("template image exceeds the 134217728-byte decoded bound");
    }
    cv::Mat continuous;
    decoded.copyTo(continuous);
    return continuous;
}

class PreparedTemplateMatch final : public inspection::IPreparedAlgorithm {
public:
    PreparedTemplateMatch(TemplateParameters parameters, cv::Mat template_image)
        : parameters_(std::move(parameters)),
          template_image_(std::move(template_image))
    {
    }

    core::Result<inspection::AlgorithmResult> inspect(
        const inspection::AlgorithmRequest& request) override
    {
        if (request.deadline.expired()) {
            return core::make_failure(core::Status::timeout,
                                      core::ErrorCode::algorithm_deadline_exceeded,
                                      "template.match deadline expired before preprocessing");
        }

        const cv::Mat& pixels = request.frame.pixels;
        if (pixels.empty()) {
            return frame_invalid("template.match requires a non-empty BGR8 or mono8 frame");
        }

        cv::Mat gray;
        if (pixels.type() == CV_8UC3) {
            cv::cvtColor(pixels, gray, cv::COLOR_BGR2GRAY);
        } else if (pixels.type() == CV_8UC1) {
            gray = pixels;
        } else {
            return frame_invalid("template.match requires a BGR8 or mono8 frame");
        }

        const cv::Rect roi(parameters_.roi_x, parameters_.roi_y, parameters_.roi_width,
                           parameters_.roi_height);
        if (roi.x < 0 || roi.y < 0 || roi.x + roi.width > gray.cols ||
            roi.y + roi.height > gray.rows) {
            return frame_invalid("template.match roi lies outside the captured frame");
        }
        if (roi.width < template_image_.cols || roi.height < template_image_.rows) {
            return frame_invalid("template.match roi is smaller than the template");
        }

        if (request.deadline.expired()) {
            return core::make_failure(core::Status::timeout,
                                      core::ErrorCode::algorithm_deadline_exceeded,
                                      "template.match deadline expired before matchTemplate");
        }

        const cv::Mat search = gray(roi);
        cv::Mat scores;
        cv::matchTemplate(search, template_image_, scores, cv::TM_CCOEFF_NORMED);

        double score = 0.0;
        cv::Point location;
        cv::minMaxLoc(scores, nullptr, &score, nullptr, &location);
        if (!std::isfinite(score)) {
            score = 0.0;
        }

        if (request.deadline.expired()) {
            return core::make_failure(core::Status::timeout,
                                      core::ErrorCode::algorithm_deadline_exceeded,
                                      "template.match deadline expired before returning");
        }

        inspection::AlgorithmResult result;
        result.measurements = nlohmann::json{
            {"score", score},
            {"x", parameters_.roi_x + location.x},
            {"y", parameters_.roi_y + location.y},
            {"width", template_image_.cols},
            {"height", template_image_.rows},
        };
        if (score >= parameters_.threshold) {
            result.verdict = core::Verdict::pass;
            result.defects = nlohmann::json::array();
        } else {
            result.verdict = core::Verdict::fail;
            result.defects = nlohmann::json::array(
                {nlohmann::json{{"kind", "template_mismatch"},
                                {"score", score},
                                {"threshold", parameters_.threshold}}});
        }
        result.diagnostics = "template.match score=" + std::to_string(score) +
                             " threshold=" + std::to_string(parameters_.threshold) +
                             " x=" + std::to_string(parameters_.roi_x + location.x) +
                             " y=" + std::to_string(parameters_.roi_y + location.y);
        return result;
    }

private:
    TemplateParameters parameters_;
    cv::Mat template_image_;
};

}  // namespace

std::string_view TemplateMatchAlgorithm::key() const noexcept
{
    return k_key;
}

core::Result<void> TemplateMatchAlgorithm::validate_parameters(const nlohmann::json& parameters) const
{
    auto parsed = parse_parameters(parameters);
    if (!parsed.has_value()) {
        return parsed.failure();
    }
    return core::Result<void>{};
}

core::Result<std::unique_ptr<inspection::IPreparedAlgorithm>> TemplateMatchAlgorithm::prepare(
    const nlohmann::json& parameters, const inspection::AlgorithmAssetBundle& assets) const
{
    auto parsed = parse_parameters(parameters);
    if (!parsed.has_value()) {
        return parsed.failure();
    }

    const inspection::AlgorithmAsset* asset = assets.find(parsed.value().template_asset);
    if (asset == nullptr) {
        return parameters_invalid("template_asset does not name a recipe asset: " +
                                  parsed.value().template_asset);
    }

    core::Result<cv::Mat> decoded = decode_grayscale(asset->bytes);
    if (!decoded.has_value()) {
        return decoded.failure();
    }
    cv::Mat template_image = std::move(decoded).value();

    if (template_image.cols > parsed.value().roi_width ||
        template_image.rows > parsed.value().roi_height) {
        return parameters_invalid("template is larger than the roi");
    }

    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(template_image, mean, deviation);
    if (!(deviation[0] > 0.0) || !std::isfinite(deviation[0])) {
        return parameters_invalid("template has zero variance");
    }

    return std::unique_ptr<inspection::IPreparedAlgorithm>{
        new PreparedTemplateMatch(std::move(parsed).value(), std::move(template_image))};
}

core::Result<inspection::AlgorithmResult> TemplateMatchAlgorithm::inspect(
    const inspection::AlgorithmRequest& request)
{
    /*
     * Legacy path: build a transient prepared object so the algorithm stays
     * usable through the pre-preparation dispatch seam. Recipe dispatch always
     * goes through prepare() and performs no I/O here.
     */
    auto parsed = parse_parameters(request.parameters);
    if (!parsed.has_value()) {
        return parsed.failure();
    }
    return core::make_failure(core::Status::algorithm_error, core::ErrorCode::algorithm_not_found,
                              "template.match requires the prepared dispatch path");
}

}  // namespace cvforwin::algorithms

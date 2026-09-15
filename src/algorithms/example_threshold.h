/*
 * Deterministic example algorithm: cv2-style grayscale threshold.
 *
 * Mirrors a small Python cv2 inspection so the mapping to C++ stays obvious:
 *
 *   gray  = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
 *   mask  = cv2.threshold(gray, threshold, 255, cv2.THRESH_BINARY)[1]
 *   mask  = cv2.bitwise_not(mask) if invert else mask
 *   ratio = cv2.countNonZero(mask) / float(mask.size)
 *   verdict = PASS if ratio >= min_pass_ratio else FAIL
 *
 * It is an example, not a production inspection algorithm: parameters are
 * validated strictly, the deadline is checked before any image work, and the
 * measurements/defects/diagnostics result stays bounded for dispatch.
 */

#ifndef CVFORWIN_SRC_ALGORITHMS_EXAMPLE_THRESHOLD_H_
#define CVFORWIN_SRC_ALGORITHMS_EXAMPLE_THRESHOLD_H_

#include <string_view>

#include "inspection/algorithm.h"

namespace cvforwin::algorithms {

class ExampleThresholdAlgorithm final : public inspection::IInspectionAlgorithm {
public:
    static constexpr std::string_view k_key = "example.threshold";
    static constexpr int k_default_threshold = 128;
    static constexpr double k_default_min_pass_ratio = 0.5;

    std::string_view key() const noexcept override;

    /* Accepts only {threshold: int 0..255, min_pass_ratio: number 0.0..1.0}. */
    core::Result<void> validate_parameters(const nlohmann::json& parameters) const override;

    /*
     * BGR8 frame -> grayscale -> binary mask (gray > threshold) -> optional
     * invert; PASS when white_pixels / total_pixels >= min_pass_ratio.
     */
    core::Result<inspection::AlgorithmResult> inspect(const inspection::AlgorithmRequest& request) override;
};

}  // namespace cvforwin::algorithms

#endif /* CVFORWIN_SRC_ALGORITHMS_EXAMPLE_THRESHOLD_H_ */

/*
 * template.match reference algorithm.
 *
 * Recipe-time preparation resolves one logical template asset from the bounded
 * bundle, decodes .png/.jpg/.jpeg bytes exactly once with OpenCV imgcodecs,
 * normalizes the template to grayscale, and stores the decoded template plus
 * the validated scalars in an immutable prepared object. inspect() performs no
 * disk I/O: it converts the frame to grayscale, restricts the search to the
 * validated ROI, and returns the ccoeff_normed best match as global-frame
 * coordinates. The verdict is PASS exactly when score >= threshold, otherwise
 * FAIL with a deterministic template_mismatch defect.
 */

#ifndef CVFORWIN_SRC_ALGORITHMS_TEMPLATE_MATCH_H_
#define CVFORWIN_SRC_ALGORITHMS_TEMPLATE_MATCH_H_

#include <memory>
#include <string_view>

#include "inspection/algorithm.h"

namespace cvforwin::algorithms {

class TemplateMatchAlgorithm final : public inspection::IInspectionAlgorithm {
public:
    static constexpr std::string_view k_key = "template.match";

    std::string_view key() const noexcept override;

    core::Result<void> validate_parameters(const nlohmann::json& parameters) const override;

    core::Result<inspection::AlgorithmResult> inspect(
        const inspection::AlgorithmRequest& request) override;

    core::Result<std::unique_ptr<inspection::IPreparedAlgorithm>> prepare(
        const nlohmann::json& parameters,
        const inspection::AlgorithmAssetBundle& assets) const override;
};

}  // namespace cvforwin::algorithms

#endif /* CVFORWIN_SRC_ALGORITHMS_TEMPLATE_MATCH_H_ */

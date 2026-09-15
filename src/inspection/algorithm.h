/*
 * Inspection algorithm authoring contract.
 *
 * An algorithm receives one owned captured frame, immutable recipe
 * parameters, optional request input JSON, and the remaining end-to-end
 * deadline. It never opens a camera, selects a recipe, persists a file,
 * invokes a host callback, or touches the public C ABI: it returns a product
 * verdict plus bounded measurements/defects JSON and a short diagnostic
 * string. All three types are internal and may evolve with the compiled
 * algorithms that implement them.
 */

#ifndef CVFORWIN_SRC_INSPECTION_ALGORITHM_H_
#define CVFORWIN_SRC_INSPECTION_ALGORITHM_H_

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "camera/captured_frame.h"
#include "core/deadline.h"
#include "core/result.h"
#include "core/status.h"

namespace cvforwin::inspection {

struct AlgorithmRequest {
    const camera::CapturedFrame& frame;
    const nlohmann::json& parameters;
    const std::optional<nlohmann::json>& input_json;
    core::Deadline deadline;
};

struct AlgorithmResult {
    core::Verdict verdict = core::Verdict::not_evaluated;
    nlohmann::json measurements = nlohmann::json::object();
    nlohmann::json defects = nlohmann::json::array();
    std::string diagnostics;
};

class IInspectionAlgorithm {
public:
    virtual ~IInspectionAlgorithm() = default;

    /* Stable lowercase registry key, e.g. "example.threshold". */
    virtual std::string_view key() const noexcept = 0;

    /* Strictly validates one recipe parameters object before dispatch. */
    virtual core::Result<void> validate_parameters(const nlohmann::json& parameters) const = 0;

    /*
     * Inspects one frame and honors the request deadline; dispatch contains
     * any escaped exception rather than letting it cross the boundary.
     */
    virtual core::Result<AlgorithmResult> inspect(const AlgorithmRequest& request) = 0;
};

}  // namespace cvforwin::inspection

#endif /* CVFORWIN_SRC_INSPECTION_ALGORITHM_H_ */

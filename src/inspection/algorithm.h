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

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

/*
 * One bounded recipe asset owned by the candidate recipe snapshot: its logical
 * key, the validated normalized relative reference (diagnostics only), and the
 * immutable encoded byte content. Bytes are loaded once before preparation and
 * are never re-read during inspect.
 */
struct AlgorithmAsset {
    std::string key;
    std::string reference;
    std::vector<std::uint8_t> bytes;
};

/*
 * Immutable bounded in-memory asset bundle handed to preparation. The bundle
 * is a plain value: it lives with the candidate recipe snapshot, never as a
 * process-global cache.
 */
class AlgorithmAssetBundle {
public:
    AlgorithmAssetBundle() = default;

    explicit AlgorithmAssetBundle(std::vector<AlgorithmAsset> assets_in)
        : assets(std::move(assets_in))
    {
    }

    /* Resolves one logical asset key, or nullptr when it is absent. */
    const AlgorithmAsset* find(std::string_view key) const noexcept
    {
        for (const AlgorithmAsset& asset : assets) {
            if (asset.key == key) {
                return &asset;
            }
        }
        return nullptr;
    }

    std::vector<AlgorithmAsset> assets;
};

/*
 * Immutable per-recipe prepared state. It is created before catalog
 * publication and never mutated during inspect, so one prepared object is safe
 * for concurrent inspections and remains valid while its recipe snapshot is
 * alive.
 */
class IPreparedAlgorithm {
public:
    virtual ~IPreparedAlgorithm() = default;

    /* Inspects one frame using the recipe-time prepared state. */
    virtual core::Result<AlgorithmResult> inspect(const AlgorithmRequest& request) = 0;
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

    /*
     * Prepares immutable per-recipe state from immutable parameters and a
     * bounded asset bundle. The default implementation validates the
     * parameters, rejects a non-empty bundle (an algorithm that needs assets
     * must override prepare), and returns a forwarding prepared object that
     * calls inspect(), so algorithms without assets need no change.
     */
    virtual core::Result<std::unique_ptr<IPreparedAlgorithm>> prepare(
        const nlohmann::json& parameters, const AlgorithmAssetBundle& assets) const;
};

}  // namespace cvforwin::inspection

#endif /* CVFORWIN_SRC_INSPECTION_ALGORITHM_H_ */

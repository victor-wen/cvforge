#include "inspection/algorithm.h"

#include <memory>
#include <utility>

#include "core/error.h"

namespace cvforwin::inspection {

namespace {

constexpr std::string_view k_no_assets_supported =
    "this algorithm declares no assets and rejects a non-empty asset bundle";

/*
 * Default prepared object: forwards every inspection to the algorithm's
 * existing inspect(). It owns no resources and performs no preparation, so an
 * algorithm that keeps its parameters in the request needs no prepare().
 */
class ForwardingPreparedAlgorithm final : public IPreparedAlgorithm {
public:
    explicit ForwardingPreparedAlgorithm(IInspectionAlgorithm& algorithm) noexcept
        : algorithm_(algorithm)
    {
    }

    core::Result<AlgorithmResult> inspect(const AlgorithmRequest& request) override
    {
        return algorithm_.inspect(request);
    }

private:
    IInspectionAlgorithm& algorithm_;
};

}  // namespace

core::Result<std::unique_ptr<IPreparedAlgorithm>> IInspectionAlgorithm::prepare(
    const nlohmann::json& parameters, const AlgorithmAssetBundle& assets) const
{
    core::Result<void> validation = validate_parameters(parameters);
    if (!validation.has_value()) {
        return validation.failure();
    }
    if (!assets.assets.empty()) {
        return core::make_failure(core::Status::config_error,
                                  core::ErrorCode::algorithm_parameters_invalid,
                                  std::string(k_no_assets_supported));
    }
    return std::unique_ptr<IPreparedAlgorithm>{
        new ForwardingPreparedAlgorithm(const_cast<IInspectionAlgorithm&>(*this))};
}

}  // namespace cvforwin::inspection

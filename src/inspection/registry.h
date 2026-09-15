/*
 * Compiled algorithm registry and dispatch containment.
 *
 * The registry owns the compiled algorithms, keyed by a stable lowercase
 * identifier ([a-z0-9._-]{1,64}). Keys are unique and are never replaced;
 * dispatching an unknown key is always a failure. dispatch() narrows the
 * algorithm boundary: it checks the deadline before invoking the algorithm,
 * rejects empty frames, converts any escaped exception to a stable failure,
 * and rejects results whose measurements plus defects exceed
 * k_max_result_json_bytes.
 *
 * Registration is compile-time only: no runtime plug-in loading exists in
 * baseline 1.0.0.
 */

#ifndef CVFORWIN_SRC_INSPECTION_REGISTRY_H_
#define CVFORWIN_SRC_INSPECTION_REGISTRY_H_

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "inspection/algorithm.h"

namespace cvforwin::inspection {

/* Maximum serialized size of measurements + defects accepted from an algorithm. */
constexpr std::size_t k_max_result_json_bytes = 65536;

class AlgorithmRegistry {
public:
    /* Registers one algorithm by key; the registry takes ownership. */
    core::Result<void> add(std::unique_ptr<IInspectionAlgorithm> algorithm);

    /* Resolves a registered algorithm; unknown keys fail with algorithm_not_found. */
    core::Result<const IInspectionAlgorithm*> find(std::string_view key) const;

    /* Registered keys in registration order. */
    std::vector<std::string> keys() const;

    std::size_t size() const noexcept;
    bool empty() const noexcept;

private:
    std::vector<std::unique_ptr<IInspectionAlgorithm>> algorithms_;
};

/*
 * Invokes one algorithm under the contract rules above. The algorithm's own
 * Failure is propagated unchanged; every other failure originates here.
 */
core::Result<AlgorithmResult> dispatch(const IInspectionAlgorithm& algorithm, const AlgorithmRequest& request);

}  // namespace cvforwin::inspection

#endif /* CVFORWIN_SRC_INSPECTION_REGISTRY_H_ */

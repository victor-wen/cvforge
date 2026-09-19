#include "algorithms/compiled_algorithms.h"

#include <memory>

#include "algorithms/example_threshold.h"
#include "algorithms/template_match.h"

namespace cvforwin::algorithms {

core::Result<void> register_compiled_algorithms(inspection::AlgorithmRegistry& registry)
{
    auto added = registry.add(std::make_unique<ExampleThresholdAlgorithm>());
    if (!added.has_value()) {
        return added.failure();
    }
    return registry.add(std::make_unique<TemplateMatchAlgorithm>());
}

}  // namespace cvforwin::algorithms

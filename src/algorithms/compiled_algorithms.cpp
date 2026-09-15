#include "algorithms/compiled_algorithms.h"

#include <memory>

#include "algorithms/example_threshold.h"

namespace cvforwin::algorithms {

core::Result<void> register_compiled_algorithms(inspection::AlgorithmRegistry& registry)
{
    return registry.add(std::make_unique<ExampleThresholdAlgorithm>());
}

}  // namespace cvforwin::algorithms

/*
 * Compiled algorithm registration.
 *
 * Baseline 1.0.0 compiles algorithms into the library; there is no runtime
 * plug-in loading. register_compiled_algorithms() publishes every compiled
 * algorithm into the registry and propagates the first registration failure.
 */

#ifndef CVFORWIN_SRC_ALGORITHMS_COMPILED_ALGORITHMS_H_
#define CVFORWIN_SRC_ALGORITHMS_COMPILED_ALGORITHMS_H_

#include "inspection/registry.h"

namespace cvforwin::algorithms {

/* Registers every compiled algorithm; currently exactly "example.threshold". */
core::Result<void> register_compiled_algorithms(inspection::AlgorithmRegistry& registry);

}  // namespace cvforwin::algorithms

#endif /* CVFORWIN_SRC_ALGORITHMS_COMPILED_ALGORITHMS_H_ */

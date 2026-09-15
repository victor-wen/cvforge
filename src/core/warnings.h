/*
 * Internal inspection warning flags.
 *
 * These bits are the internal, ABI-independent representation of the public
 * cvf_inspection_result_v1.warning_flags values that the runtime/c_api change
 * copies into the caller-owned result. The public C header is deliberately not
 * included here, so core_model stays independent of the public boundary.
 *
 * A warning records a contained optional-sink failure and never changes a
 * valid product verdict.
 */

#ifndef CVFORWIN_SRC_CORE_WARNINGS_H_
#define CVFORWIN_SRC_CORE_WARNINGS_H_

#include <cstdint>

namespace cvforwin::core {

enum WarningFlags : std::uint32_t {
    warning_none = 0,
    warning_log_sink_failed = 1u << 0,
    warning_image_save_failed = 1u << 1,
};

}  // namespace cvforwin::core

#endif /* CVFORWIN_SRC_CORE_WARNINGS_H_ */

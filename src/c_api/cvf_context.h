/*
 * Internal opaque-context definition.
 *
 * This header is private to src/c_api and is never installed or shipped. The
 * public header only forward-declares cvf_context, so the layout below is a
 * DLL-internal detail that the host can never depend on.
 *
 * The process holds at most one live context. The single context pointer is
 * guarded by a process-wide mutex; only cvf_initialize publishes it and only
 * cvf_shutdown invalidates it. The runtime orchestrator owns the camera
 * session, recipe snapshot, diagnostics, artifacts, and serialization gate
 * behind this boundary.
 */

#ifndef CVFORWIN_SRC_C_API_CVF_CONTEXT_H_
#define CVFORWIN_SRC_C_API_CVF_CONTEXT_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <cvforwin/cvf_api.h>

#include "runtime/context.h"

namespace cvforwin::capi {

inline constexpr std::uint32_t kContextMagic = 0x4356464Du; /* "CVFM" */

struct ContextState {
    std::uint32_t magic = kContextMagic;
    std::uint32_t abi_version = CVF_ABI_VERSION_V1;
    std::string config_root;
    std::string output_root;
    std::uint32_t flags = 0u;
    cvf_log_callback log_callback = nullptr;
    void* user_data = nullptr;
    std::unique_ptr<runtime::Context> runtime;
};

/* Process-wide context registry. */
std::mutex& context_mutex() noexcept;
ContextState*& live_context_slot() noexcept;

/* True only for the one pointer published by cvf_initialize. */
bool is_live_context(const cvf_context* context) noexcept;

}  // namespace cvforwin::capi

struct cvf_context {
    cvforwin::capi::ContextState state;
};

#endif /* CVFORWIN_SRC_C_API_CVF_CONTEXT_H_ */

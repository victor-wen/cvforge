/*
 * Process-wide context registry for the C API boundary.
 *
 * CVF-001 never publishes a context: cvf_initialize always fails before the
 * publish step because the configuration/recipe/camera subsystems are out of
 * scope. The registry still exists so the "at most one live context" rule has a
 * concrete enforcement point, and so a later change cannot accidentally skip
 * it. The slot is only ever written while holding context_mutex().
 */

#include "c_api/cvf_context.h"

namespace cvforwin::capi {
namespace {

std::mutex g_context_mutex;
ContextState* g_live_context = nullptr;

}  // namespace

std::mutex& context_mutex() noexcept
{
    return g_context_mutex;
}

ContextState*& live_context_slot() noexcept
{
    return g_live_context;
}

bool is_live_context(const cvf_context* context) noexcept
{
    if (context == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> guard(g_context_mutex);
    return g_live_context == &context->state && context->state.magic == kContextMagic;
}

}  // namespace cvforwin::capi

/*
 * Runtime orchestrator interface.
 *
 * The Context owns the single process runtime: the immutable recipe snapshot,
 * the compiled algorithm registry, diagnostics, the managed capture store, the
 * camera session, and one timed serialization gate. create() initializes in
 * dependency order and rolls back every already-created resource on failure;
 * inspect() executes one complete cycle under one end-to-end deadline; and
 * reload_recipes() atomically replaces the recipe snapshot only after a
 * complete candidate catalog validates.
 *
 * The single-live-context rule is enforced by the public C API; the internal
 * runtime can be exercised directly by tests.
 */

#ifndef CVFORWIN_SRC_RUNTIME_CONTEXT_H_
#define CVFORWIN_SRC_RUNTIME_CONTEXT_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "camera/camera_backend.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"
#include "diagnostics/diagnostics.h"

namespace cvforwin::runtime {

/*
 * Inspection timeout in milliseconds. The value keeps the frozen millisecond
 * count while accepting both an integral millisecond count and a
 * std::chrono::milliseconds duration, so either spelling carries the same
 * observable deadline. Zero means the documented 5000 ms default.
 */
class TimeoutMs {
public:
    TimeoutMs() noexcept = default;
    TimeoutMs(std::uint32_t milliseconds) noexcept
        : milliseconds_(milliseconds)
    {}
    TimeoutMs(std::chrono::milliseconds duration) noexcept
        : milliseconds_(static_cast<std::uint32_t>(duration.count()))
    {
    }

    std::uint32_t value() const noexcept
    {
        return milliseconds_;
    }

private:
    std::uint32_t milliseconds_ = 0u;
};

struct RuntimeOptions {
    std::filesystem::path config_root;
    std::filesystem::path output_root;
    bool enable_file_logging = true;
    bool enable_callback_logging = false;
    diagnostics::CallbackBinding log_callback{};
    /* Testing hook; when set, config-driven backend construction is skipped. */
    std::shared_ptr<camera::ICameraBackend> camera_override;
};

struct InspectionRequest {
    std::string recipe_id;
    std::string request_id;
    TimeoutMs timeout_ms;
    std::optional<std::string> input_json;
};

struct InspectionOutcome {
    core::Status status = core::Status::ok;
    core::Verdict verdict = core::Verdict::not_evaluated;
    core::ErrorCode error_code = core::ErrorCode::none;
    std::uint32_t warning_flags = 0;
    std::uint32_t elapsed_ms = 0;
    /* Measurements plus defects as one bounded JSON object; null when cleared. */
    nlohmann::json output_json;
    std::string image_path;
    std::string error_message;
};

class Context {
public:
    /*
     * Loads the global configuration and all recipes, creates diagnostics and
     * the managed capture store, builds or resolves and opens the camera, and
     * publishes the context last. Any failure destroys every already-created
     * resource and returns its Failure.
     */
    static core::Result<std::unique_ptr<Context>> create(RuntimeOptions options);

    /*
     * One complete inspection cycle under one deadline covering the serialized
     * queue wait, recipe lookup, capture with at most one reconnect/recapture,
     * algorithm dispatch, encoding, and required persistence. Every execution
     * outcome is a valued result; only a closed context returns a Failure.
     */
    core::Result<InspectionOutcome> inspect(const InspectionRequest& request);

    /* Validates a complete candidate recipe catalog and atomically publishes it. */
    core::Result<void> reload_recipes();

    /* Accumulated warning bits (diagnostics sinks plus per-call artifact warnings). */
    std::uint32_t warning_flags() const noexcept;

    /* Orderly shutdown of the camera, diagnostics, and artifacts. */
    ~Context();

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

private:
    class Impl;

    explicit Context(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace cvforwin::runtime

#endif /* CVFORWIN_SRC_RUNTIME_CONTEXT_H_ */

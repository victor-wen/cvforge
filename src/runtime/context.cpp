/*
 * Runtime orchestrator implementation.
 *
 * create() initializes in dependency order (global config, recipes,
 * diagnostics, managed captures, camera) and relies on RAII for a complete
 * rollback: every resource is owned by a local smart pointer until the Context
 * is published last, so a failure destroys exactly what was created.
 *
 * inspect() runs one complete cycle under one Deadline: the timed serialization
 * gate, recipe lookup, capture with at most one reconnect/recapture, algorithm
 * dispatch, result encoding, and required persistence each receive only the
 * time that remains. Every execution outcome is a valued InspectionOutcome;
 * technical errors always report NOT_EVALUATED and clear the JSON/image path.
 *
 * reload_recipes() validates a complete candidate catalog and publishes it with
 * one atomic store, so a concurrent reader observes the old or the new catalog.
 */

#include "runtime/context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "algorithms/compiled_algorithms.h"
#include "artifacts/capture_store.h"
#include "core/deadline.h"
#include "core/warnings.h"
#include "inspection/algorithm.h"
#include "inspection/registry.h"
#include "recipes/config.h"
#include "recipes/recipe.h"
#include "recipes/recipe_catalog.h"

#if defined(CVFORWIN_TEST_BACKENDS_ENABLED) && CVFORWIN_TEST_BACKENDS_ENABLED
#include "camera/test_backends/file_camera_backend.h"
#include "camera/test_backends/synthetic_camera_backend.h"
#endif

#if defined(_WIN32)
#include "camera/uvc_windows/uvc_backend.h"
#endif

namespace cvforwin::runtime {

namespace {

constexpr std::uint32_t k_default_timeout_ms = 5000u;
constexpr std::uint64_t k_seconds_per_day = 86400u;

core::LogLevel log_level_from_token(std::string_view token) noexcept
{
    if (token == "trace") {
        return core::LogLevel::trace;
    }
    if (token == "debug") {
        return core::LogLevel::debug;
    }
    if (token == "warn") {
        return core::LogLevel::warn;
    }
    if (token == "error") {
        return core::LogLevel::error;
    }
    if (token == "critical") {
        return core::LogLevel::critical;
    }
    return core::LogLevel::info;
}

camera::PixelFormat pixel_format_from_token(std::string_view token) noexcept
{
    if (token == "mono8") {
        return camera::PixelFormat::mono8;
    }
    if (token == "bgr8") {
        return camera::PixelFormat::bgr8;
    }
    if (token == "rgb8") {
        return camera::PixelFormat::rgb8;
    }
    return camera::PixelFormat::unknown;
}

camera::CameraSettings base_settings(const recipes::GlobalConfig& config)
{
    camera::CameraSettings settings;
    settings.width = config.base_capture.width;
    settings.height = config.base_capture.height;
    settings.frame_rate = config.base_capture.frame_rate;
    settings.preferred_format = pixel_format_from_token(config.base_capture.pixel_format);
    return settings;
}

camera::CameraSettings recipe_settings(const recipes::GlobalConfig& config, const recipes::Recipe& recipe)
{
    camera::CameraSettings settings;
    settings.width = recipe.capture.width.value_or(config.base_capture.width);
    settings.height = recipe.capture.height.value_or(config.base_capture.height);
    settings.frame_rate = recipe.capture.frame_rate.value_or(config.base_capture.frame_rate);
    const std::string pixel_format =
        recipe.capture.pixel_format.has_value() ? recipe.capture.pixel_format.value() : config.base_capture.pixel_format;
    settings.preferred_format = pixel_format_from_token(pixel_format);
    return settings;
}

std::uint32_t elapsed_ms_since(std::chrono::steady_clock::time_point started) noexcept
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    if (elapsed <= 0) {
        return 0u;
    }
    const auto count = static_cast<std::uintmax_t>(elapsed);
    if (count > std::numeric_limits<std::uint32_t>::max()) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(count);
}

/*
 * Regular files directly under the configured frames directory, sorted by
 * filename. A missing or unreadable directory fails initialization; an empty
 * existing directory is a valid camera that fails captures with
 * frames_exhausted.
 */
core::Result<std::vector<std::string>> list_frame_files(const std::filesystem::path& frames_dir)
{
    std::error_code error;
    const bool exists = std::filesystem::exists(frames_dir, error);
    if (error || !exists) {
        return core::make_failure(core::Status::camera_not_found, core::ErrorCode::camera_not_found,
                                  "file camera frames directory does not exist: " + frames_dir.string());
    }
    const bool directory = std::filesystem::is_directory(frames_dir, error);
    if (error || !directory) {
        return core::make_failure(core::Status::camera_not_found, core::ErrorCode::camera_not_found,
                                  "file camera device_path is not a directory: " + frames_dir.string());
    }

    std::vector<std::filesystem::path> files;
    std::filesystem::directory_iterator iterator(frames_dir, error);
    if (error) {
        return core::make_failure(core::Status::camera_io, core::ErrorCode::capture_failed,
                                  "file camera frames directory cannot be read: " + frames_dir.string());
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        std::error_code entry_error;
        if (iterator->is_regular_file(entry_error) && !entry_error) {
            files.push_back(iterator->path());
        }
        iterator.increment(error);
        if (error) {
            return core::make_failure(core::Status::camera_io, core::ErrorCode::capture_failed,
                                      "file camera frames directory cannot be read: " + frames_dir.string());
        }
    }

    std::sort(files.begin(), files.end(), [](const std::filesystem::path& left, const std::filesystem::path& right) {
        return left.filename().string() < right.filename().string();
    });

    std::vector<std::string> paths;
    paths.reserve(files.size());
    for (const std::filesystem::path& file : files) {
        paths.push_back(file.string());
    }
    return paths;
}

/*
 * Config-driven backend construction. "file" and "synthetic" exist only in
 * builds with the deterministic test backends enabled; "uvc" selects the
 * Windows Media Foundation backend on Windows builds and fails with
 * camera_not_found on portable builds, where the module is not compiled.
 */
core::Result<std::shared_ptr<camera::ICameraBackend>> build_backend(const recipes::GlobalConfig& config)
{
#if defined(CVFORWIN_TEST_BACKENDS_ENABLED) && CVFORWIN_TEST_BACKENDS_ENABLED
    if (config.camera.backend == "synthetic") {
        camera::SyntheticCameraConfig backend_config{
            .descriptor = camera::CameraDescriptor{.backend_key = "synthetic",
                                                   .device_path = "synthetic0",
                                                   .vendor_id = "0000",
                                                   .product_id = "0000",
                                                   .friendly_name = "Synthetic camera"},
            .width = config.base_capture.width,
            .height = config.base_capture.height,
        };
        return std::shared_ptr<camera::ICameraBackend>{
            std::make_shared<camera::SyntheticCameraBackend>(std::move(backend_config))};
    }
    if (config.camera.backend == "file") {
        const std::filesystem::path frames_dir(config.camera.device_path);
        if (!frames_dir.is_absolute()) {
            return core::make_failure(core::Status::config_error, core::ErrorCode::config_value_invalid,
                                      "file camera device_path must be an absolute frames directory");
        }
        core::Result<std::vector<std::string>> frames = list_frame_files(frames_dir);
        if (!frames.has_value()) {
            return frames.failure();
        }
        camera::FileCameraConfig backend_config{
            .descriptor = camera::CameraDescriptor{.backend_key = "file",
                                                   .device_path = frames_dir.string(),
                                                   .vendor_id = config.camera.vendor_id,
                                                   .product_id = config.camera.product_id,
                                                   .friendly_name = config.camera.friendly_name},
            .frame_paths = std::move(frames).value(),
        };
        return std::shared_ptr<camera::ICameraBackend>{
            std::make_shared<camera::FileCameraBackend>(std::move(backend_config))};
    }
#endif
    if (config.camera.backend == "uvc") {
#if defined(_WIN32)
        return std::shared_ptr<camera::ICameraBackend>{std::make_shared<camera::UvcCameraBackend>()};
#else
        return core::make_failure(core::Status::camera_not_found, core::ErrorCode::camera_not_found,
                                  "the uvc camera backend is only available in Windows Media Foundation builds");
#endif
    }
    return core::make_failure(core::Status::config_error, core::ErrorCode::config_value_invalid,
                              "unsupported camera backend: " + config.camera.backend);
}

nlohmann::json serialize_measurements(const inspection::AlgorithmResult& result)
{
    nlohmann::json output = result.measurements.is_object() ? result.measurements : nlohmann::json::object();
    if (!result.defects.empty()) {
        output["defects"] = result.defects;
    }
    return output;
}

}  // namespace

class Context::Impl {
public:
    Impl(recipes::GlobalConfig config, inspection::AlgorithmRegistry registry,
         std::shared_ptr<const recipes::RecipeCatalog> catalog,
         std::unique_ptr<diagnostics::Diagnostics> diagnostics,
         std::unique_ptr<artifacts::CaptureStore> captures,
         std::shared_ptr<camera::ICameraBackend> camera)
        : config_(std::move(config)),
          registry_(std::move(registry)),
          diagnostics_(std::move(diagnostics)),
          captures_(std::move(captures)),
          camera_(std::move(camera)),
          captures_root_(config_.output_root / "captures")
    {
        recipes_.publish(std::move(catalog));
    }

    ~Impl()
    {
        closed_.store(true, std::memory_order_release);
        if (camera_) {
            camera_->close();
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void log_startup()
    {
        diagnostics_->log(core::LogLevel::info, "runtime context initialized");
    }

    core::Result<InspectionOutcome> inspect(const InspectionRequest& request)
    {
        if (closed_.load(std::memory_order_acquire)) {
            return core::make_failure(core::Status::invalid_context, core::ErrorCode::runtime_context_closed,
                                      "the runtime context is closed");
        }

        const auto started = std::chrono::steady_clock::now();
        const std::uint32_t timeout_ms =
            request.timeout_ms.value() == 0u ? k_default_timeout_ms : request.timeout_ms.value();
        const core::Deadline deadline = core::Deadline::from_timeout_ms(timeout_ms);

        std::optional<nlohmann::json> algorithm_input;
        if (request.input_json.has_value()) {
            nlohmann::json parsed = nlohmann::json::parse(request.input_json.value(), nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                return failure_outcome(core::make_failure(core::Status::invalid_argument,
                                                          core::ErrorCode::request_invalid,
                                                          "input_json must be a valid UTF-8 JSON object"),
                                       started);
            }
            algorithm_input = std::move(parsed);
        }

        std::unique_lock<std::timed_mutex> gate(gate_, std::defer_lock);
        if (!gate.try_lock_for(deadline.remaining())) {
            return failure_outcome(
                core::make_failure(core::Status::timeout, core::ErrorCode::runtime_queue_timeout,
                                   "inspection deadline expired while waiting for the serialization gate"),
                started);
        }

        const std::shared_ptr<const recipes::RecipeCatalog> snapshot = recipes_.current();
        if (!snapshot) {
            return failure_outcome(core::make_failure(core::Status::internal_error,
                                                      core::ErrorCode::internal_unexpected,
                                                      "the recipe snapshot has not been published"),
                                   started);
        }
        const core::Result<const recipes::Recipe*> recipe_result = snapshot->find(request.recipe_id);
        if (!recipe_result.has_value()) {
            return failure_outcome(recipe_result.failure(), started);
        }
        const recipes::Recipe& recipe = *recipe_result.value();

        const camera::CameraSettings settings = recipe_settings(config_, recipe);
        core::Result<camera::CapturedFrame> captured = camera::capture_with_one_retry(*camera_, settings, deadline);
        if (!captured.has_value()) {
            return failure_outcome(captured.failure(), started);
        }
        camera::CapturedFrame frame = std::move(captured).value();
        for (std::uint32_t settled = 0; settled < recipe.capture.settle_frames; ++settled) {
            if (deadline.expired()) {
                return failure_outcome(core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                                          "inspection deadline expired while settling the capture"),
                                       started);
            }
            core::Result<camera::CapturedFrame> next = camera_->capture(deadline);
            if (!next.has_value()) {
                return failure_outcome(next.failure(), started);
            }
            frame = std::move(next).value();
        }

        const core::Result<const inspection::IInspectionAlgorithm*> algorithm_result =
            registry_.find(recipe.algorithm);
        if (!algorithm_result.has_value()) {
            return failure_outcome(algorithm_result.failure(), started);
        }
        const inspection::AlgorithmRequest algorithm_request{frame, recipe.parameters, algorithm_input, deadline};
        core::Result<inspection::AlgorithmResult> dispatched =
            inspection::dispatch(*algorithm_result.value(), algorithm_request);
        if (!dispatched.has_value()) {
            return failure_outcome(dispatched.failure(), started);
        }
        const inspection::AlgorithmResult algorithm_outcome = std::move(dispatched).value();

        InspectionOutcome outcome;
        outcome.status = core::Status::ok;
        outcome.verdict = algorithm_outcome.verdict;
        outcome.error_code = core::ErrorCode::none;
        try {
            outcome.output_json = serialize_measurements(algorithm_outcome);
        } catch (const std::exception&) {
            return failure_outcome(core::make_failure(core::Status::internal_error,
                                                      core::ErrorCode::internal_exception,
                                                      "algorithm measurements could not be serialized"),
                                   started);
        }

        const artifacts::SaveDecision decision =
            artifacts::decide_capture_save(recipe.artifacts.save_policy, outcome.verdict, true);
        if (decision == artifacts::SaveDecision::save) {
            const artifacts::CaptureSaveRequest save_request{frame.pixels, recipe.recipe_id, request.request_id,
                                                             frame.metadata.sequence};
            core::Result<std::filesystem::path> saved = captures_->save(save_request);
            if (saved.has_value()) {
                outcome.image_path = saved.value().string();
            } else if (recipe.artifacts.required) {
                outcome.status = core::Status::required_artifact_error;
                outcome.verdict = core::Verdict::not_evaluated;
                outcome.error_code = core::ErrorCode::runtime_required_artifact_failed;
                outcome.output_json = nlohmann::json();
                outcome.image_path.clear();
                outcome.error_message = "required capture persistence failed: " + saved.failure().message;
                outcome.warning_flags = warning_flags();
                outcome.elapsed_ms = elapsed_ms_since(started);
                return outcome;
            } else {
                add_warning(core::warning_image_save_failed);
            }
        }

        /* Retention runs after saves; a failure is an optional-artifact warning. */
        const std::chrono::seconds max_age{
            static_cast<std::chrono::seconds::rep>(config_.retention.max_age_days * k_seconds_per_day)};
        const core::Result<std::uint32_t> retained =
            artifacts::CaptureStore::enforce_retention(captures_root_, max_age, config_.retention.max_total_bytes);
        if (!retained.has_value()) {
            add_warning(core::warning_image_save_failed);
        }

        outcome.warning_flags = warning_flags();
        outcome.elapsed_ms = elapsed_ms_since(started);
        return outcome;
    }

    core::Result<void> reload_recipes()
    {
        if (closed_.load(std::memory_order_acquire)) {
            return core::make_failure(core::Status::invalid_context, core::ErrorCode::runtime_context_closed,
                                      "the runtime context is closed");
        }
        core::Result<recipes::RecipeCatalog> candidate =
            recipes::RecipeCatalog::load(config_.config_root / "recipes", registry_);
        if (!candidate.has_value()) {
            const std::string message = "recipe reload rejected: " + candidate.failure().message;
            diagnostics_->log(core::LogLevel::warn, message);
            return core::make_failure(core::Status::config_error, core::ErrorCode::runtime_reload_failed, message);
        }
        recipes_.publish(std::make_shared<const recipes::RecipeCatalog>(std::move(candidate).value()));
        diagnostics_->log(core::LogLevel::info, "recipe snapshot reloaded");
        return core::Result<void>{};
    }

    std::uint32_t warning_flags() const noexcept
    {
        return warnings_.load(std::memory_order_relaxed) | diagnostics_->warnings();
    }

private:
    InspectionOutcome failure_outcome(const core::Failure& failure,
                                      std::chrono::steady_clock::time_point started) const
    {
        InspectionOutcome outcome;
        outcome.status = failure.status;
        outcome.verdict = core::Verdict::not_evaluated;
        outcome.error_code = failure.code;
        outcome.warning_flags = warning_flags();
        outcome.elapsed_ms = elapsed_ms_since(started);
        outcome.error_message = failure.message;
        return outcome;
    }

    void add_warning(core::WarningFlags flag) noexcept
    {
        warnings_.fetch_or(static_cast<std::uint32_t>(flag), std::memory_order_relaxed);
    }

    recipes::GlobalConfig config_;
    inspection::AlgorithmRegistry registry_;
    recipes::RecipeHolder recipes_;
    std::unique_ptr<diagnostics::Diagnostics> diagnostics_;
    std::unique_ptr<artifacts::CaptureStore> captures_;
    std::shared_ptr<camera::ICameraBackend> camera_;
    std::filesystem::path captures_root_;
    std::atomic<bool> closed_{false};
    std::atomic<std::uint32_t> warnings_{0u};
    std::timed_mutex gate_;
};

Context::Context(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl))
{
}

Context::~Context() = default;

core::Result<std::unique_ptr<Context>> Context::create(RuntimeOptions options)
{
    core::Result<recipes::GlobalConfig> config_result =
        recipes::load_global_config(options.config_root, options.output_root);
    if (!config_result.has_value()) {
        return config_result.failure();
    }
    recipes::GlobalConfig config = std::move(config_result).value();

    inspection::AlgorithmRegistry registry;
    core::Result<void> registered = algorithms::register_compiled_algorithms(registry);
    if (!registered.has_value()) {
        return registered.failure();
    }

    core::Result<recipes::RecipeCatalog> catalog_result =
        recipes::RecipeCatalog::load(options.config_root / "recipes", registry);
    if (!catalog_result.has_value()) {
        return catalog_result.failure();
    }
    std::shared_ptr<const recipes::RecipeCatalog> catalog =
        std::make_shared<const recipes::RecipeCatalog>(std::move(catalog_result).value());

    diagnostics::DiagnosticsConfig diagnostics_config;
    diagnostics_config.level = log_level_from_token(config.logging.level);
    diagnostics_config.max_file_bytes = config.logging.max_file_bytes;
    diagnostics_config.max_files = config.logging.max_files;
    diagnostics_config.log_dir = options.output_root / "logs";
    diagnostics_config.enable_file_sink = options.enable_file_logging;
    diagnostics::CallbackBinding callback{};
    if (options.enable_callback_logging) {
        callback = options.log_callback;
    }
    core::Result<std::unique_ptr<diagnostics::Diagnostics>> diagnostics_result =
        diagnostics::Diagnostics::create(diagnostics_config, callback);
    if (!diagnostics_result.has_value()) {
        return diagnostics_result.failure();
    }
    std::unique_ptr<diagnostics::Diagnostics> diagnostics = std::move(diagnostics_result).value();

    core::Result<std::unique_ptr<artifacts::CaptureStore>> captures_result =
        artifacts::CaptureStore::create(options.output_root / "captures");
    if (!captures_result.has_value()) {
        return captures_result.failure();
    }
    std::unique_ptr<artifacts::CaptureStore> captures = std::move(captures_result).value();

    std::shared_ptr<camera::ICameraBackend> camera;
    if (options.camera_override) {
        camera = std::move(options.camera_override);
    } else {
        core::Result<std::shared_ptr<camera::ICameraBackend>> built = build_backend(config);
        if (!built.has_value()) {
            return built.failure();
        }
        camera = std::move(built).value();
    }

    const core::Deadline deadline = core::Deadline::from_timeout_ms(k_default_timeout_ms);
    core::Result<std::vector<camera::CameraDescriptor>> candidates = camera->enumerate(deadline);
    if (!candidates.has_value()) {
        return candidates.failure();
    }
    const camera::CameraSelector selector{config.camera.device_path, config.camera.vendor_id,
                                          config.camera.product_id, config.camera.friendly_name};
    core::Result<camera::CameraDescriptor> descriptor_result =
        camera::resolve_identity(candidates.value(), selector);
    if (!descriptor_result.has_value()) {
        return descriptor_result.failure();
    }
    camera::CameraDescriptor descriptor = std::move(descriptor_result).value();

    const camera::CameraSettings settings = base_settings(config);
    core::Result<void> opened = camera->open(descriptor, settings, deadline);
    if (!opened.has_value()) {
        return opened.failure();
    }

    std::unique_ptr<Context::Impl> impl =
        std::make_unique<Context::Impl>(std::move(config), std::move(registry), std::move(catalog),
                                        std::move(diagnostics), std::move(captures), std::move(camera));
    impl->log_startup();
    return std::unique_ptr<Context>(new Context(std::move(impl)));
}

core::Result<InspectionOutcome> Context::inspect(const InspectionRequest& request)
{
    return impl_->inspect(request);
}

core::Result<void> Context::reload_recipes()
{
    return impl_->reload_recipes();
}

std::uint32_t Context::warning_flags() const noexcept
{
    return impl_->warning_flags();
}

}  // namespace cvforwin::runtime

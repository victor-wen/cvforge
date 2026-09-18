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
#include <condition_variable>
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
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "algorithms/compiled_algorithms.h"
#include "artifacts/capture_sink.h"
#include "artifacts/capture_store.h"
#include "artifacts/retention_scheduler.h"
#include "artifacts/save_worker.h"
#include "core/deadline.h"
#include "core/warnings.h"
#include "inspection/algorithm.h"
#include "inspection/registry.h"
#include "recipes/config.h"
#include "recipes/recipe.h"
#include "recipes/recipe_catalog.h"
#include "runtime/finalization.h"

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

core::Result<std::string> serialize_result_payload(const nlohmann::json& output)
{
    std::string serialized;
    try {
        serialized = output.dump();
    } catch (const std::exception&) {
        return core::make_failure(core::Status::internal_error, core::ErrorCode::internal_exception,
                                  "the inspection result could not be serialized");
    }
    if (serialized.size() > k_max_result_payload_bytes) {
        return core::make_failure(core::Status::buffer_too_small, core::ErrorCode::runtime_result_too_large,
                                  "the serialized inspection result exceeds the bounded payload");
    }
    return serialized;
}

class Context::Impl {
public:
    Impl(recipes::GlobalConfig config, inspection::AlgorithmRegistry registry,
         std::shared_ptr<const recipes::RecipeCatalog> catalog,
         std::unique_ptr<diagnostics::Diagnostics> diagnostics,
         std::unique_ptr<artifacts::CaptureStore> captures,
         std::shared_ptr<camera::ICameraBackend> camera,
         std::shared_ptr<artifacts::ArtifactSink> injected_sink)
        : config_(std::move(config)),
          registry_(std::move(registry)),
          diagnostics_(std::move(diagnostics)),
          captures_(std::move(captures)),
          camera_(std::move(camera)),
          captures_root_(config_.output_root / "captures"),
          sink_(injected_sink ? std::move(injected_sink)
                              : std::shared_ptr<artifacts::ArtifactSink>(
                                    std::make_shared<artifacts::CaptureStoreSink>(*captures_))),
          save_worker_(*sink_)
    {
        recipes_.publish(std::move(catalog));
        /* Anchor the retention time trigger at construction so the first run
         * is not scheduled merely because the steady-clock epoch is in the
         * past. */
        retention_.on_retention_run(std::chrono::steady_clock::now());
        retention_root_ = captures_root_;
        maintenance_ = std::thread([this] { maintenance_loop(); });
    }

    ~Impl()
    {
        closed_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(maintenance_mutex_);
            maintenance_stop_ = true;
            maintenance_requested_ = false;
            maintenance_cv_.notify_all();
        }
        if (maintenance_.joinable()) {
            maintenance_.join();
        }
        /* Completes or cancels in-flight work and never deadlocks. */
        save_worker_.drain();
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
        /* One absolute deadline for the whole cycle. Every stage below receives
         * this exact Deadline and never computes its own timeout. */
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

        /*
         * One finalization request carries every post-frame input. The latest
         * valid frame stays alive here until finalize_after_frame has produced
         * the single decision.
         */
        FinalizeRequest fin;
        fin.requirement = save_requirement_for(recipe.artifacts.save_policy, recipe.artifacts.required);

        const camera::CameraSettings settings = recipe_settings(config_, recipe);
        core::Result<camera::CapturedFrame> captured = camera::capture_with_one_retry(*camera_, settings, deadline);
        if (!captured.has_value()) {
            /* No valid frame ever existed: no artifact attempt is allowed. */
            return failure_outcome(captured.failure(), started);
        }
        camera::CapturedFrame frame = std::move(captured).value();
        fin.frame_valid = true;
        for (std::uint32_t settled = 0; settled < recipe.capture.settle_frames; ++settled) {
            if (deadline.expired()) {
                const core::Failure missed_settle =
                    core::make_failure(core::Status::timeout, core::ErrorCode::deadline_expired,
                                       "inspection deadline expired while settling the capture");
                return finalize_post_frame(&missed_settle, deadline, recipe, request, frame, fin,
                                           nlohmann::json(), started);
            }
            core::Result<camera::CapturedFrame> next = camera_->capture(deadline);
            if (!next.has_value()) {
                return finalize_post_frame(&next.failure(), deadline, recipe, request, frame, fin, nlohmann::json(), started);
            }
            frame = std::move(next).value();
        }

        const core::Result<const inspection::IInspectionAlgorithm*> algorithm_result =
            registry_.find(recipe.algorithm);
        if (!algorithm_result.has_value()) {
            return finalize_post_frame(&algorithm_result.failure(), deadline, recipe, request, frame, fin,
                                       nlohmann::json(), started);
        }
        const inspection::AlgorithmRequest algorithm_request{frame, recipe.parameters, algorithm_input, deadline};
        core::Result<inspection::AlgorithmResult> dispatched =
            inspection::dispatch(*algorithm_result.value(), algorithm_request);
        if (!dispatched.has_value()) {
            return finalize_post_frame(&dispatched.failure(), deadline, recipe, request, frame, fin, nlohmann::json(), started);
        }
        const inspection::AlgorithmResult algorithm_outcome = std::move(dispatched).value();
        fin.verdict = algorithm_outcome.verdict;
        fin.execution_ok = true;

        nlohmann::json output_json;
        std::string output_text;
        try {
            output_json = serialize_measurements(algorithm_outcome);
            core::Result<std::string> serialized = serialize_result_payload(output_json);
            if (!serialized.has_value()) {
                return finalize_post_frame(&serialized.failure(), deadline, recipe, request, frame, fin,
                                           nlohmann::json(), started);
            }
            output_text = std::move(serialized).value();
        } catch (const std::exception&) {
            const core::Failure serialize_error =
                core::make_failure(core::Status::internal_error, core::ErrorCode::internal_exception,
                                   "algorithm measurements could not be serialized");
            return finalize_post_frame(&serialize_error, deadline, recipe, request, frame, fin,
                                       nlohmann::json(), started);
        }

        return finalize_post_frame(nullptr, deadline, recipe, request, frame, fin, std::move(output_json), started,
                                   std::move(output_text));
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
    /*
     * Single exit for every outcome after a valid frame: algorithm failure,
     * serialization failure, or success. It applies the recipe artifact policy
     * with the real execution_ok value, waits on the bounded save worker only
     * until the absolute deadline, then builds the public outcome from exactly
     * one finalize_after_frame decision.
     */
    InspectionOutcome finalize_post_frame(const core::Failure* failure, const core::Deadline& deadline,
                                          const recipes::Recipe& recipe, const InspectionRequest& request,
                                          const camera::CapturedFrame& frame, FinalizeRequest fin,
                                          nlohmann::json output_json,
                                          std::chrono::steady_clock::time_point started,
                                          std::string output_text = {})
    {
        if (failure != nullptr) {
            fin.execution_ok = false;
            fin.verdict = core::Verdict::not_evaluated;
            fin.post_frame_status = failure->status != core::Status::ok ? failure->status : core::Status::internal_error;
            fin.post_frame_error = failure->code;
        }

        const artifacts::SaveDecision policy =
            artifacts::decide_capture_save(recipe.artifacts.save_policy, fin.verdict, fin.execution_ok);
        if (policy != artifacts::SaveDecision::save) {
            fin.requirement = SaveRequirement::none;
        }

        std::filesystem::path saved_path;
        if (policy == artifacts::SaveDecision::save && fin.frame_valid) {
            artifacts::SaveJob job;
            /* Own the pixels so the worker never observes a caller-owned Mat. */
            job.pixels = frame.pixels.clone();
            job.recipe_id = recipe.recipe_id;
            job.request_id = request.request_id;
            job.sequence = frame.metadata.sequence;

            fin.save_attempted = true;
            if (save_worker_.try_submit(job)) {
                const artifacts::SaveOutcome saved = save_worker_.wait_until(deadline);
                if (saved.state == artifacts::SaveState::completed) {
                    fin.save_committed = true;
                    saved_path = saved.path;
                } else {
                    fin.save_failed = true;
                    /* Drop this inspection's queued job; the executing one
                     * cannot be interrupted without a caller-visible handle. */
                    save_worker_.cancel_pending();
                }
            } else {
                fin.save_failed = true;
                /* A required save that cannot even be queued is awaited only to
                 * the absolute deadline, after which it is a timeout. */
                if (fin.requirement == SaveRequirement::required) {
                    while (!deadline.expired()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
            }
        }

        fin.deadline_expired = deadline.expired() && !fin.save_committed;

        const FinalizeDecision decision = finalize_after_frame(fin, warning_flags());

        InspectionOutcome outcome;
        outcome.status = decision.status;
        outcome.verdict = decision.verdict;
        outcome.error_code = decision.error_code;
        outcome.warning_flags = decision.warning_flags;
        outcome.elapsed_ms = elapsed_ms_since(started);
        if (decision.status == core::Status::ok) {
            outcome.output_json = std::move(output_json);
            outcome.output_text = std::move(output_text);
        } else {
            outcome.output_json = nlohmann::json();
            outcome.output_text.clear();
            if (failure != nullptr) {
                outcome.error_message = failure->message;
            } else if (decision.status == core::Status::required_artifact_error) {
                outcome.error_message = "required capture persistence failed before the deadline";
            } else if (decision.status == core::Status::timeout) {
                outcome.error_message =
                    "inspection deadline expired before the required capture was persisted";
            }
        }
        if (decision.publish_image_path && !saved_path.empty()) {
            outcome.image_path = saved_path.string();
        }
        if ((decision.warning_flags & static_cast<std::uint32_t>(core::warning_image_save_failed)) != 0u) {
            add_warning(core::warning_image_save_failed);
        }

        schedule_retention(fin.save_committed, saved_path);
        return outcome;
    }

    /*
     * Records one committed capture and requests a coalesced background
     * retention run when either trigger fires. It never scans, sorts, or
     * deletes; the maintenance worker owns that work.
     */
    void schedule_retention(bool committed, const std::filesystem::path& saved_path)
    {
        bool request = false;
        {
            std::lock_guard<std::mutex> lock(maintenance_mutex_);
            if (committed && !saved_path.empty()) {
                const std::filesystem::path parent = saved_path.parent_path();
                if (!parent.empty()) {
                    retention_root_ = parent;
                }
            }
            const auto now = std::chrono::steady_clock::now();
            if (committed) {
                request = retention_.on_capture_committed(now);
            }
            if (!request) {
                request = retention_.due(now);
            }
            if (request) {
                maintenance_requested_ = true;
            }
        }
        if (request) {
            maintenance_cv_.notify_all();
        }
    }

    void maintenance_loop()
    {
        std::unique_lock<std::mutex> lock(maintenance_mutex_);
        while (!maintenance_stop_) {
            if (!maintenance_requested_) {
                const auto now = std::chrono::steady_clock::now();
                if (retention_.due(now)) {
                    maintenance_requested_ = true;
                }
            }
            if (!maintenance_requested_) {
                maintenance_cv_.wait_for(lock, std::chrono::milliseconds(200));
                continue;
            }

            maintenance_requested_ = false;
            const std::chrono::seconds max_age{
                static_cast<std::chrono::seconds::rep>(config_.retention.max_age_days * k_seconds_per_day)};
            const std::uint64_t max_total_bytes = config_.retention.max_total_bytes;
            const std::filesystem::path root = retention_root_;
            lock.unlock();
            const core::Result<std::uint32_t> retained =
                artifacts::CaptureStore::enforce_retention(root, max_age, max_total_bytes);
            lock.lock();
            if (!retained.has_value()) {
                add_warning(core::warning_image_save_failed);
            }
            retention_.on_retention_run(std::chrono::steady_clock::now());
        }
    }

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

    /* Artifact execution: one bounded save worker plus a separate maintenance
     * worker for coalesced retention. Neither runs on the inspect caller. */
    std::shared_ptr<artifacts::ArtifactSink> sink_;
    artifacts::SaveWorker save_worker_;
    artifacts::RetentionScheduler retention_;
    std::mutex maintenance_mutex_;
    std::condition_variable maintenance_cv_;
    bool maintenance_stop_ = false;
    bool maintenance_requested_ = false;
    std::filesystem::path retention_root_;
    std::thread maintenance_;

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
                                        std::move(diagnostics), std::move(captures), std::move(camera),
                                        std::move(options.artifact_sink));
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

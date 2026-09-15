#pragma once

// CVF-006 independent black-box test support (owner: test-engineer).
//
// Shared aliases, config/recipe writers, the deterministic synthetic-backend
// harness, outcome accessors, and timing helpers for the independent
// runtime-orchestrator suite (brief B7-B12). This header compiles only against
// the frozen interface headers listed in the CVF-006 test brief; it never
// includes production .cpp files and never inspects private state.
//
// Interface spellings used here are derived from the CVF-006 brief's frozen
// interface_reference; every assumption beyond that text is recorded in
// .ai/reports/CVF-006-test-red.yaml (author assumptions).
//
// Frozen interface headers first: a missing interface header must be the first
// diagnostic in the author (RED) phase.

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"
#include "core/warnings.h"

#include "runtime/context.h"

#include "camera/camera_backend.h"
#include "camera/captured_frame.h"
#include "camera/test_backends/synthetic_camera_backend.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cvf006 {

namespace core = cvforwin::core;
namespace cam = cvforwin::camera;
namespace rt = cvforwin::runtime;

using Json = nlohmann::json;

// Frame size used by every internal runtime case; the same size is written to
// both the global config base_capture block and the recipe capture overrides so
// the captured frame size is unambiguous.
inline constexpr int kFrameWidth = 16;
inline constexpr int kFrameHeight = 12;

// Brief B10/warning contract: log sink failure = 1u << 0, image save failure =
// 1u << 1. Compared numerically so the test does not depend on enumerator names.
inline constexpr std::uint32_t kLogSinkWarningBit = 1u << 0u;
inline constexpr std::uint32_t kImageSaveWarningBit = 1u << 1u;

// --- run-time temporary directories ----------------------------------------
//
// Same unique-counter pattern as the CVF-004/CVF-005 suites: created fresh and
// removed on destruction, so cases never share state or depend on leftovers.

class TempDir {
public:
    explicit TempDir(std::string_view tag)
    {
        namespace fs = std::filesystem;
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        path_ = fs::temp_directory_path() / ("cvf006_" + std::string(tag) + "_" + unique);
        std::error_code error;
        fs::remove_all(path_, error);
        if (!fs::create_directories(path_, error)) {
            throw std::runtime_error("cvf006: cannot create temp directory " + path_.string());
        }
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    ~TempDir()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

// --- synthetic camera -------------------------------------------------------

// Brief backend_config: synthetic descriptor device_path "synthetic0",
// vendor_id/product_id "0000", friendly_name "Synthetic camera".
inline cam::CameraDescriptor synthetic_descriptor()
{
    return cam::CameraDescriptor{
        .backend_key = "synthetic",
        .device_path = "synthetic0",
        .vendor_id = "0000",
        .product_id = "0000",
        .friendly_name = "Synthetic camera",
    };
}

inline std::shared_ptr<cam::SyntheticCameraBackend> make_synthetic_backend(
    int width = kFrameWidth, int height = kFrameHeight)
{
    return std::make_shared<cam::SyntheticCameraBackend>(
        cam::SyntheticCameraConfig{
            .descriptor = synthetic_descriptor(),
            .width = width,
            .height = height,
        });
}

// CVF-002 B4 documented synthetic pixel: BGR((x+y+seq)&0xFF, (x+2y+seq)&0xFF,
// (2x+y+seq)&0xFF). Used to prove a saved image really is the captured frame.
inline cv::Vec3b synthetic_pixel(std::uint64_t sequence, int x, int y)
{
    const auto byte = [](std::uint64_t value) {
        return static_cast<unsigned char>(value & 0xFFu);
    };
    return cv::Vec3b(byte(static_cast<std::uint64_t>(x + y) + sequence),
                     byte(static_cast<std::uint64_t>(x + 2 * y) + sequence),
                     byte(static_cast<std::uint64_t>(2 * x + y) + sequence));
}

// --- configuration and recipe documents -------------------------------------

inline Json camera_block(const char* backend, const std::string& device_path)
{
    return Json{
        {"backend", backend},
        {"device_path", device_path},
        {"vendor_id", "0000"},
        {"product_id", "0000"},
        {"friendly_name", backend == std::string("synthetic") ? "Synthetic camera" : "File camera"},
    };
}

inline Json base_capture_block(int width, int height)
{
    return Json{
        {"width", width},
        {"height", height},
        {"frame_rate", 30.0},
        {"pixel_format", "bgr8"},
    };
}

inline Json synthetic_config(int width = kFrameWidth, int height = kFrameHeight)
{
    return Json{
        {"schema_version", 1},
        {"camera", camera_block("synthetic", "synthetic0")},
        {"base_capture", base_capture_block(width, height)},
        {"logging", Json{{"level", "info"}, {"max_file_bytes", 1048576}, {"max_files", 2}}},
        {"retention", Json{{"max_age_days", 30}, {"max_total_bytes", 1073741824}}},
    };
}

// Brief backend_config: file descriptor device_path = absolute frames directory.
inline Json file_config(const std::filesystem::path& frames_dir, int width = kFrameWidth,
                        int height = kFrameHeight)
{
    return Json{
        {"schema_version", 1},
        {"camera", camera_block("file", frames_dir.string())},
        {"base_capture", base_capture_block(width, height)},
        {"logging", Json{{"level", "info"}, {"max_file_bytes", 1048576}, {"max_files", 2}}},
        {"retention", Json{{"max_age_days", 30}, {"max_total_bytes", 1073741824}}},
    };
}

inline Json pass_parameters()
{
    // Boundary rule: pass_ratio >= min_pass_ratio passes, so 0.0 always passes.
    return Json{{"threshold", 128}, {"min_pass_ratio", 0.0}};
}

inline Json fail_parameters()
{
    // With threshold 255 only pure-white pixels count; the synthetic frame has
    // black pixels (pixel (0,0) for sequence 0 is BGR(0,0,0)), so a required
    // full pass ratio can never be met -> FAIL.
    return Json{{"threshold", 255}, {"min_pass_ratio", 1.0}};
}

inline Json recipe_document(const std::string& recipe_id, const Json& parameters,
                            const char* save_policy = "never", bool required = false,
                            int width = kFrameWidth, int height = kFrameHeight)
{
    return Json{
        {"schema_version", 1},
        {"recipe_id", recipe_id},
        {"algorithm", "example.threshold"},
        {"parameters", parameters},
        {"capture",
         Json{{"width", width}, {"height", height}, {"frame_rate", 15.0}, {"pixel_format", "bgr8"}, {"settle_frames", 0}}},
        {"artifacts", Json{{"save_policy", save_policy}, {"required", required}}},
    };
}

inline void write_text(const std::filesystem::path& file, const std::string& text)
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

// Config root layout fixed by the contract: <config_root>/cvforwin.json and
// <config_root>/recipes/*.json.
inline void write_config(const std::filesystem::path& config_root, const Json& config)
{
    write_text(config_root / "cvforwin.json", config.dump(2));
}

inline void write_recipe(const std::filesystem::path& config_root, const std::string& filename,
                         const Json& recipe)
{
    std::error_code error;
    std::filesystem::create_directories(config_root / "recipes", error);
    REQUIRE_FALSE(error);
    write_text(config_root / "recipes" / filename, recipe.dump(2));
}

// --- runtime options and context --------------------------------------------

inline rt::RuntimeOptions runtime_options(const std::filesystem::path& config_root,
                                          const std::filesystem::path& output_root,
                                          bool enable_file_logging = false)
{
    rt::RuntimeOptions options{};
    options.config_root = config_root.string();
    options.output_root = output_root.string();
    options.enable_file_logging = enable_file_logging;
    options.enable_callback_logging = false;
    return options;
}

// camera_override is the brief's documented fault-injection path: the CVF-002
// test backends are passed through RuntimeOptions.
inline void set_camera_override(rt::RuntimeOptions& options,
                                std::shared_ptr<cam::SyntheticCameraBackend> backend)
{
    options.camera_override = std::move(backend);
}

inline std::unique_ptr<rt::Context> create_context(const rt::RuntimeOptions& options)
{
    auto created = rt::Context::create(options);
    REQUIRE(created.has_value());
    return std::move(created.value());
}

// --- requests ---------------------------------------------------------------
//
// The holder owns the identifier storage so the request remains valid for the
// whole call regardless of whether the frozen field type is owning or a view
// (same lifetime pattern as the CVF-005 SaveRequestHolder).

class RequestHolder {
public:
    RequestHolder(std::string recipe_id, std::string request_id, std::uint32_t timeout_ms)
        : recipe_storage_(std::move(recipe_id)), request_storage_(std::move(request_id))
    {
        request_.recipe_id = recipe_storage_;
        request_.request_id = request_storage_;
        assign_timeout(timeout_ms);
    }

    RequestHolder(const RequestHolder&) = delete;
    RequestHolder& operator=(const RequestHolder&) = delete;

    const rt::InspectionRequest& get() const& noexcept
    {
        return request_;
    }
    const rt::InspectionRequest& get() const&& = delete;

private:
    void assign_timeout(std::uint32_t timeout_ms)
    {
        if constexpr (requires { request_.timeout_ms = timeout_ms; }) {
            request_.timeout_ms = timeout_ms;
        } else {
            request_.timeout_ms = std::chrono::milliseconds(timeout_ms);
        }
    }

    std::string recipe_storage_;
    std::string request_storage_;
    rt::InspectionRequest request_{};
};

// --- inspecting -------------------------------------------------------------

// Accepts both an outcome-returning inspect and a Result-returning inspect so
// the suite compiles against the documented value shape either way.
inline bool try_capture_outcome(const rt::InspectionOutcome& result, rt::InspectionOutcome& out)
{
    out = result;
    return true;
}

inline bool try_capture_outcome(const core::Result<rt::InspectionOutcome>& result,
                                rt::InspectionOutcome& out)
{
    if (!result.has_value()) {
        return false;
    }
    out = result.value();
    return true;
}

inline rt::InspectionOutcome inspect_with(const std::unique_ptr<rt::Context>& context,
                                          const rt::InspectionRequest& request)
{
    auto result = context->inspect(request);
    rt::InspectionOutcome outcome{};
    const bool captured = try_capture_outcome(result, outcome);
    REQUIRE(captured);
    return outcome;
}

// --- outcome accessors ------------------------------------------------------

template <typename Flags>
inline std::uint32_t warning_bits(Flags flags)
{
    return static_cast<std::uint32_t>(flags);
}

// elapsed_ms may be an integral millisecond count or a chrono duration.
inline std::int64_t elapsed_ms_of(const rt::InspectionOutcome& outcome)
{
    return static_cast<std::int64_t>(outcome.elapsed_ms);
}

inline std::string output_text(const rt::InspectionOutcome& outcome)
{
    if constexpr (requires { outcome.output_json.dump(); }) {
        return outcome.output_json.dump();
    } else {
        return std::string(outcome.output_json);
    }
}

inline Json output_json_of(const rt::InspectionOutcome& outcome)
{
    if constexpr (requires { outcome.output_json.is_object(); }) {
        return outcome.output_json;
    } else {
        return Json::parse(std::string(outcome.output_json));
    }
}

// A cleared output_json is empty/null (contract: non-OK inspect clears it).
inline bool output_cleared(const rt::InspectionOutcome& outcome)
{
    return outcome.output_json.empty();
}

inline std::filesystem::path image_file_of(const rt::InspectionOutcome& outcome)
{
    return std::filesystem::path(outcome.image_path);
}

// --- harness ----------------------------------------------------------------

class SyntheticHarness {
public:
    SyntheticHarness(std::string_view tag, const std::string& recipe_id, const Json& parameters,
                     const char* save_policy = "never", bool required = false)
        : config_root_(std::string(tag) + "_config"), output_root_(std::string(tag) + "_output")
    {
        backend_ = make_synthetic_backend();
        write_config(config_root_.path(), synthetic_config());
        write_recipe(config_root_.path(), "recipe.json",
                     recipe_document(recipe_id, parameters, save_policy, required));
        auto options = runtime_options(config_root_.path(), output_root_.path());
        set_camera_override(options, backend_);
        context_ = create_context(options);
    }

    const std::shared_ptr<cam::SyntheticCameraBackend>& backend() const noexcept
    {
        return backend_;
    }

    const std::unique_ptr<rt::Context>& context() const noexcept
    {
        return context_;
    }

    const std::filesystem::path& output_root() const noexcept
    {
        return output_root_.path();
    }

    rt::InspectionOutcome inspect(const rt::InspectionRequest& request) const
    {
        return inspect_with(context_, request);
    }

private:
    TempDir config_root_;
    TempDir output_root_;
    std::shared_ptr<cam::SyntheticCameraBackend> backend_;
    std::unique_ptr<rt::Context> context_;
};

// --- timing -----------------------------------------------------------------

inline std::int64_t wall_ms_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

// --- file-system failure injection ------------------------------------------

// Replaces an existing path with a regular file at the same location. Any later
// write below that path fails (ENOTDIR / not-a-directory) even for a privileged
// user, which is how save failures are injected without guessing internal
// subdirectory names.
inline void replace_with_blocking_file(const std::filesystem::path& path, std::string_view tag)
{
    std::error_code error;
    std::filesystem::remove_all(path, error);
    REQUIRE_FALSE(std::filesystem::exists(path));
    std::ofstream blocker(path, std::ios::binary | std::ios::trunc);
    REQUIRE(blocker.good());
    blocker << tag;
    REQUIRE(blocker.good());
}

}  // namespace cvf006

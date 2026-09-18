#pragma once

// CVF-104 independent black-box test support (owner: test-engineer).
//
// Shared aliases, run-time temporary directories, deterministic synthetic
// camera/config/recipe builders, a deterministic ArtifactSink test double, and
// the runtime harness for the CVF-104 end-to-end cases. This header compiles
// only against the frozen seam/interface headers named in the CVF-104 brief; it
// never includes production .cpp files and never inspects private state.
//
// Frozen seam headers first: a missing seam header must be the first diagnostic
// in the author (RED) phase.

#include "core/deadline.h"
#include "core/error.h"
#include "core/result.h"
#include "core/status.h"
#include "core/warnings.h"

#include "artifacts/capture_store.h"
#include "artifacts/retention_scheduler.h"
#include "artifacts/save_worker.h"
#include "runtime/context.h"
#include "runtime/finalization.h"

#include "camera/camera_backend.h"
#include "camera/captured_frame.h"
#include "camera/test_backends/synthetic_camera_backend.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace cvf104 {

namespace core = cvforwin::core;
namespace rt = cvforwin::runtime;
namespace art = cvforwin::artifacts;
namespace cam = cvforwin::camera;

using Json = nlohmann::json;

// Frame size shared by the runtime cases; written to both the global
// base_capture block and the recipe capture overrides.
inline constexpr int kFrameWidth = 16;
inline constexpr int kFrameHeight = 12;

// Frozen warning bit values (core/warnings.h): log sink failure = 1u << 0,
// image save failure = 1u << 1. Compared numerically so a case does not depend
// on the enumerator spelling.
inline constexpr std::uint32_t kLogSinkWarningBit = 1u << 0u;
inline constexpr std::uint32_t kImageSaveWarningBit = 1u << 1u;

inline bool has_image_warning(std::uint32_t flags) noexcept
{
    return (flags & kImageSaveWarningBit) != 0u;
}

inline bool has_log_warning(std::uint32_t flags) noexcept
{
    return (flags & kLogSinkWarningBit) != 0u;
}

// --- run-time temporary directories ----------------------------------------

class TempDir {
public:
    explicit TempDir(std::string_view tag)
    {
        namespace fs = std::filesystem;
        static std::atomic<std::uint64_t> counter{0};
        const auto unique = std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        path_ = fs::temp_directory_path() / ("cvf104_" + std::string(tag) + "_" + unique);
        std::error_code error;
        fs::remove_all(path_, error);
        if (!fs::create_directories(path_, error)) {
            throw std::runtime_error("cvf104: cannot create temp directory " + path_.string());
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

// --- file helpers -----------------------------------------------------------

inline void write_bytes(const std::filesystem::path& file, std::size_t bytes, char filler = 'x')
{
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << std::string(bytes, filler);
    REQUIRE(stream.good());
}

inline void set_age(const std::filesystem::path& file, std::chrono::seconds age)
{
    std::error_code error;
    std::filesystem::last_write_time(file,
                                     std::filesystem::file_time_type::clock::now() - age, error);
    REQUIRE_FALSE(error);
}

inline std::vector<std::filesystem::path> regular_files_under(const std::filesystem::path& dir)
{
    std::vector<std::filesystem::path> files;
    std::error_code error;
    if (!std::filesystem::is_directory(dir, error)) {
        return files;
    }
    std::filesystem::recursive_directory_iterator iterator(dir, error);
    REQUIRE_FALSE(error);
    for (const auto& entry : iterator) {
        if (entry.is_regular_file(error)) {
            files.push_back(entry.path());
        }
    }
    return files;
}

inline std::size_t count_regular_files(const std::filesystem::path& dir)
{
    return regular_files_under(dir).size();
}

inline std::size_t count_files_with_extension(const std::filesystem::path& dir,
                                              std::string_view extension)
{
    std::size_t count = 0;
    for (const auto& file : regular_files_under(dir)) {
        if (file.extension() == extension) {
            ++count;
        }
    }
    return count;
}

// True when child is equal to, or a descendant of, root (both absolute).
inline bool path_within(const std::filesystem::path& root, const std::filesystem::path& child)
{
    const std::string root_text = root.lexically_normal().string();
    const std::string child_text = child.lexically_normal().string();
    if (child_text.size() < root_text.size()) {
        return false;
    }
    if (child_text.compare(0, root_text.size(), root_text) != 0) {
        return false;
    }
    return child_text.size() == root_text.size() || child_text[root_text.size()] == '/';
}

// --- synthetic camera -------------------------------------------------------

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
            .width = static_cast<std::uint32_t>(width),
            .height = static_cast<std::uint32_t>(height),
        });
}

// --- configuration and recipe documents -------------------------------------

inline Json synthetic_config(int max_age_days = 30, std::uint64_t max_total_bytes = 1073741824ull)
{
    return Json{
        {"schema_version", 1},
        {"camera",
         Json{{"backend", "synthetic"},
              {"device_path", "synthetic0"},
              {"vendor_id", "0000"},
              {"product_id", "0000"},
              {"friendly_name", "Synthetic camera"}}},
        {"base_capture",
         Json{{"width", kFrameWidth},
              {"height", kFrameHeight},
              {"frame_rate", 30.0},
              {"pixel_format", "bgr8"}}},
        {"logging", Json{{"level", "info"}, {"max_file_bytes", 1048576}, {"max_files", 2}}},
        {"retention", Json{{"max_age_days", max_age_days}, {"max_total_bytes", max_total_bytes}}},
    };
}

inline Json pass_parameters()
{
    // pass_ratio >= min_pass_ratio passes, so 0.0 always passes.
    return Json{{"threshold", 128}, {"min_pass_ratio", 0.0}};
}

inline Json fail_parameters()
{
    // Threshold 255 + required full pass ratio: the synthetic frame has black
    // pixels, so the verdict can only be FAIL.
    return Json{{"threshold", 255}, {"min_pass_ratio", 1.0}};
}

inline Json recipe_document(const std::string& recipe_id, const Json& parameters,
                            const char* save_policy = "never", bool required = false)
{
    return Json{
        {"schema_version", 1},
        {"recipe_id", recipe_id},
        {"algorithm", "example.threshold"},
        {"parameters", parameters},
        {"capture",
         Json{{"width", kFrameWidth},
              {"height", kFrameHeight},
              {"frame_rate", 15.0},
              {"pixel_format", "bgr8"},
              {"settle_frames", 0}}},
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

inline void write_config(const std::filesystem::path& config_root, const Json& config)
{
    write_text(config_root / "cvforwin.json", config.dump(2));
}

inline void write_recipe(const std::filesystem::path& config_root, const Json& recipe)
{
    std::error_code error;
    std::filesystem::create_directories(config_root / "recipes", error);
    REQUIRE_FALSE(error);
    write_text(config_root / "recipes" / "recipe.json", recipe.dump(2));
}

// --- requests and inspecting ------------------------------------------------

class RequestHolder {
public:
    RequestHolder(std::string recipe_id, std::string request_id, std::uint32_t timeout_ms)
        : recipe_storage_(std::move(recipe_id)), request_storage_(std::move(request_id))
    {
        request_.recipe_id = recipe_storage_;
        request_.request_id = request_storage_;
        request_.timeout_ms = rt::TimeoutMs(timeout_ms);
    }

    RequestHolder(const RequestHolder&) = delete;
    RequestHolder& operator=(const RequestHolder&) = delete;

    const rt::InspectionRequest& get() const& noexcept
    {
        return request_;
    }
    const rt::InspectionRequest& get() const&& = delete;

private:
    std::string recipe_storage_;
    std::string request_storage_;
    rt::InspectionRequest request_{};
};

inline rt::InspectionOutcome inspect_with(const std::unique_ptr<rt::Context>& context,
                                          const rt::InspectionRequest& request)
{
    auto result = context->inspect(request);
    REQUIRE(result.has_value());
    return std::move(result.value());
}

// --- deterministic injected ArtifactSink ------------------------------------

/*
 * Test double for the frozen ArtifactSink boundary. Modes:
 *   ambient     - encodes/writes through the real CaptureStore below its root
 *                 (a genuine PNG that commit publishes and discard removes);
 *   fail_write  - write_temp always fails with image_write_error;
 *   block_write - write_temp blocks until release() is called, then behaves like
 *                 ambient;
 *   delayed     - write_temp waits a bounded delay, then behaves like ambient.
 *
 * Every path it creates is tracked so a case can assert that no temporary was
 * orphaned (created but neither committed nor discarded).
 */
class TestSink final : public art::ArtifactSink {
public:
    enum class Mode { ambient,
                      fail_write,
                      block_write,
                      delayed };

    explicit TestSink(std::filesystem::path root, Mode mode = Mode::ambient)
        : root_(std::move(root)), mode_(mode)
    {
        std::error_code error;
        std::filesystem::create_directories(root_, error);
    }

    core::Result<std::filesystem::path> write_temp(const art::SaveJob& job) override
    {
        if (mode() == Mode::block_write) {
            {
                std::lock_guard<std::mutex> lock(gate_mutex_);
                entered_ = true;
            }
            gate_cv_.notify_all();
            std::unique_lock<std::mutex> lock(gate_mutex_);
            gate_cv_.wait(lock, [this] { return released_; });
        } else if (mode() == Mode::delayed) {
            std::this_thread::sleep_for(delay_);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        ++write_calls_;
        if (mode() == Mode::fail_write) {
            ++failed_writes_;
            return core::Failure{core::Status::internal_error, core::ErrorCode::image_write_error,
                                 "cvf104 injected write failure"};
        }

        auto store = art::CaptureStore::create(root_);
        if (!store.has_value()) {
            ++failed_writes_;
            return store.failure();
        }
        const art::CaptureSaveRequest request{job.pixels, job.recipe_id, job.request_id, job.sequence};
        auto saved = store.value()->save(request);
        if (!saved.has_value()) {
            ++failed_writes_;
            return saved.failure();
        }

        const std::string key = saved.value().string();
        temp_recipe_[key] = job.recipe_id;
        pending_.insert(key);
        return saved.value();
    }

    core::Result<std::filesystem::path> commit(const std::filesystem::path& temp) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++commit_calls_;
        const auto found = temp_recipe_.find(temp.string());
        if (found != temp_recipe_.end()) {
            committed_recipes_.push_back(found->second);
            temp_recipe_.erase(found);
        }
        pending_.erase(temp.string());
        return temp;
    }

    void discard(const std::filesystem::path& temp) noexcept override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++discard_calls_;
        const auto found = temp_recipe_.find(temp.string());
        if (found != temp_recipe_.end()) {
            discarded_recipes_.push_back(found->second);
            temp_recipe_.erase(found);
        }
        pending_.erase(temp.string());
        std::error_code error;
        std::filesystem::remove(temp, error);
    }

    /* --- gate control (block_write mode) --- */
    bool wait_entered(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(gate_mutex_);
        return gate_cv_.wait_for(lock, timeout, [this] { return entered_; });
    }

    void release()
    {
        {
            std::lock_guard<std::mutex> lock(gate_mutex_);
            released_ = true;
        }
        gate_cv_.notify_all();
    }

    void set_delay(std::chrono::milliseconds delay)
    {
        delay_ = delay;
    }

    void set_mode(Mode mode) noexcept
    {
        mode_.store(mode);
    }

    Mode mode() const noexcept
    {
        return mode_.load();
    }

    /* --- observation --- */
    std::uint32_t write_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return write_calls_;
    }

    std::uint32_t commit_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return commit_calls_;
    }

    std::uint32_t discard_calls() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return discard_calls_;
    }

    std::uint32_t failed_writes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_writes_;
    }

    std::size_t outstanding() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_.size();
    }

    std::vector<std::string> committed_recipes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return committed_recipes_;
    }

    std::vector<std::string> discarded_recipes() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return discarded_recipes_;
    }

    std::size_t committed_with_recipe(std::string_view recipe_id) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        for (const auto& id : committed_recipes_) {
            if (id == recipe_id) {
                ++count;
            }
        }
        return count;
    }

    const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

private:
    std::filesystem::path root_;
    std::atomic<Mode> mode_;
    std::chrono::milliseconds delay_{50};

    mutable std::mutex mutex_;
    std::uint32_t write_calls_ = 0;
    std::uint32_t commit_calls_ = 0;
    std::uint32_t discard_calls_ = 0;
    std::uint32_t failed_writes_ = 0;
    std::map<std::string, std::string> temp_recipe_;
    std::set<std::string> pending_;
    std::vector<std::string> committed_recipes_;
    std::vector<std::string> discarded_recipes_;

    std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    bool entered_ = false;
    bool released_ = false;
};

// --- runtime harness --------------------------------------------------------

class RuntimeHarness {
public:
    RuntimeHarness(std::string_view tag, const std::string& recipe_id, const Json& parameters,
                   const char* save_policy = "never", bool required = false,
                   int max_age_days = 30, std::uint64_t max_total_bytes = 1073741824ull,
                   std::shared_ptr<art::ArtifactSink> sink = nullptr)
        : config_root_(std::string(tag) + "_config"), output_root_(std::string(tag) + "_output")
    {
        backend_ = make_synthetic_backend();
        write_config(config_root_.path(), synthetic_config(max_age_days, max_total_bytes));
        write_recipe(config_root_.path(),
                     recipe_document(recipe_id, parameters, save_policy, required));

        rt::RuntimeOptions options{};
        options.config_root = config_root_.path().string();
        options.output_root = output_root_.path().string();
        options.enable_file_logging = false;
        options.enable_callback_logging = false;
        options.camera_override = backend_;
        if (sink != nullptr) {
            options.artifact_sink = std::move(sink);
        }

        auto created = rt::Context::create(options);
        REQUIRE(created.has_value());
        context_ = std::move(created.value());
    }

    const std::shared_ptr<cam::SyntheticCameraBackend>& backend() const noexcept
    {
        return backend_;
    }

    const std::unique_ptr<rt::Context>& context() const noexcept
    {
        return context_;
    }

    const std::filesystem::path& config_root() const noexcept
    {
        return config_root_.path();
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

}  // namespace cvf104

/*
 * CVF-007 developer-owned regression tests for the runtime UVC factory wiring.
 *
 * The UVC backend itself is Windows-only, so this file asserts what each
 * platform can actually observe:
 *
 *   - portable builds: a valid configuration with camera.backend "uvc" fails
 *     initialization with camera_not_found/camera_not_found, so the same
 *     config cannot silently fall back to a different backend;
 *   - Windows builds: UvcCameraBackend satisfies the frozen ICameraBackend
 *     shape, reports backend_key "uvc", tolerates an idempotent close on a
 *     never-opened backend, and fails capture before open with camera_io.
 *
 * The target is built and registered with CTest by the presets. The portable
 * case runs everywhere the UVC module is excluded; the Windows case pins the
 * backend interface and runs wherever the Windows target is built.
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "core/error.h"
#include "core/status.h"
#include "runtime/context.h"

#if defined(_WIN32)
#include <type_traits>

#include "camera/camera_backend.h"
#include "camera/captured_frame.h"
#include "camera/uvc_windows/uvc_backend.h"
#include "core/deadline.h"
#endif

#if !defined(_WIN32)

namespace {

class TempTree {
public:
    explicit TempTree(std::string name)
    {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = base / ("cvf007_" + std::move(name) + "_" + std::to_string(stamp));
        output_ = root_ / "output";
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_ / "recipes", error);
        std::filesystem::create_directories(output_, error);
    }

    ~TempTree()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const std::filesystem::path& root() const noexcept
    {
        return root_;
    }

    const std::filesystem::path& output() const noexcept
    {
        return output_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path output_;
};

void write_text(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.good());
    stream << text;
    stream.close();
    REQUIRE(stream.good());
}

const char* const k_uvc_config_text = R"({
  "schema_version": 1,
  "camera": {
    "backend": "uvc",
    "device_path": "usb#vid_1234&pid_5678#cvf007-developer",
    "vendor_id": "1234",
    "product_id": "5678",
    "friendly_name": "CVF-007 developer camera"
  },
  "base_capture": {
    "width": 640,
    "height": 480,
    "frame_rate": 30.0,
    "pixel_format": "bgr8"
  },
  "logging": {
    "level": "info",
    "max_file_bytes": 1048576,
    "max_files": 2
  },
  "retention": {
    "max_age_days": 30,
    "max_total_bytes": 1073741824
  }
})";

const char* const k_recipe_text = R"({
  "schema_version": 1,
  "recipe_id": "example",
  "algorithm": "example.threshold",
  "parameters": {
    "threshold": 128,
    "min_pass_ratio": 0.0
  },
  "capture": {
    "width": 640,
    "height": 480,
    "frame_rate": 30.0,
    "pixel_format": "bgr8",
    "settle_frames": 0
  },
  "artifacts": {
    "save_policy": "never",
    "required": false
  }
})";

}  // namespace

#endif /* !defined(_WIN32): the helpers above are only needed off Windows. */

#if defined(_WIN32)

TEST_CASE("CVF-007 developer: UvcCameraBackend implements the frozen backend contract", "[cvf-007][uvc]")
{
    using cvforwin::camera::CapturedFrame;
    using cvforwin::camera::ICameraBackend;
    using cvforwin::camera::UvcCameraBackend;
    using cvforwin::core::Deadline;
    using cvforwin::core::Status;

    static_assert(std::is_base_of_v<ICameraBackend, UvcCameraBackend>,
                  "UvcCameraBackend must implement cvforwin::camera::ICameraBackend");
    static_assert(std::is_default_constructible_v<UvcCameraBackend>,
                  "UvcCameraBackend must be default-constructible");

    UvcCameraBackend backend;
    CHECK(backend.backend_key() == "uvc");

    /* close() is idempotent and safe on a never-opened backend. */
    backend.close();
    backend.close();

    const auto captured = backend.capture(Deadline::from_timeout_ms(50));
    REQUIRE_FALSE(captured.has_value());
    CHECK(captured.failure().status == Status::camera_io);
    CHECK(backend.backend_key() == "uvc");
}

#else

TEST_CASE("CVF-007 developer: a uvc config fails with camera_not_found on portable builds", "[cvf-007][uvc]")
{
    using cvforwin::core::ErrorCode;
    using cvforwin::core::Status;
    using cvforwin::runtime::Context;
    using cvforwin::runtime::RuntimeOptions;

    TempTree tree("uvc_config");
    write_text(tree.root() / "cvforwin.json", k_uvc_config_text);
    write_text(tree.root() / "recipes" / "example.json", k_recipe_text);

    RuntimeOptions options;
    options.config_root = tree.root();
    options.output_root = tree.output();
    options.enable_file_logging = false;

    const auto created = Context::create(options);
    REQUIRE_FALSE(created.has_value());
    CHECK(created.failure().status == Status::camera_not_found);
    CHECK(created.failure().code == ErrorCode::camera_not_found);
    CHECK_FALSE(created.failure().message.empty());
}

#endif

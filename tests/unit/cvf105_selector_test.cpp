// CVF-105 independent black-box tests: canonical VID/PID identity matching.
//
// Brief B2: a selector using uppercase configured VID/PID resolves against a
// lowercase enumeration descriptor, while device_path and friendly_name remain
// exact byte comparisons. Per FR-024 canonicalization applies to configured and
// enumerated values *before* comparison, and the brief explicitly allows the
// behavior to be observed through configuration load or the frozen
// camera_backend selector/resolve entry point. The cases below therefore drive
// the documented pipeline (config load -> selector -> resolve) rather than
// pinning canonicalization inside one particular function, and use already
// canonical values when exercising resolve_identity's exact-match contract.
//
// Production surfaces used (frozen headers only): camera/camera_backend.h
// resolve_identity() and recipes/config.h load_global_config(); no production
// .cpp is read.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "cvf105_test.helpers.h"

#include "camera/camera_backend.h"
#include "core/error.h"
#include "core/result.h"
#include "recipes/config.h"

namespace {

namespace camera = cvforwin::camera;
namespace core = cvforwin::core;
namespace recipes = cvforwin::recipes;

camera::CameraDescriptor descriptor(std::string device_path, std::string vendor_id,
                                    std::string product_id, std::string friendly_name)
{
    camera::CameraDescriptor value;
    value.backend_key = "uvc";
    value.device_path = std::move(device_path);
    value.vendor_id = std::move(vendor_id);
    value.product_id = std::move(product_id);
    value.friendly_name = std::move(friendly_name);
    return value;
}

camera::CameraSelector selector(std::string device_path, std::string vendor_id,
                                std::string product_id, std::string friendly_name)
{
    camera::CameraSelector value;
    value.device_path = std::move(device_path);
    value.vendor_id = std::move(vendor_id);
    value.product_id = std::move(product_id);
    value.friendly_name = std::move(friendly_name);
    return value;
}

/* The selector the runtime would use after loading a config root. */
core::Result<camera::CameraSelector> loaded_selector(const std::filesystem::path& config_root,
                                                     const std::filesystem::path& output_root)
{
    core::Result<recipes::GlobalConfig> loaded =
        recipes::load_global_config(config_root, output_root);
    if (!loaded.has_value()) {
        return loaded.failure();
    }
    const recipes::GlobalConfig& config = loaded.value();
    return selector(config.camera.device_path, config.camera.vendor_id, config.camera.product_id,
                    config.camera.friendly_name);
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* B2 acceptance through the documented config-load pipeline.                 */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B2: config load stores uppercase VID/PID as four lowercase hex digits",
          "[cvf-105][B2][config]")
{
    cvf105::TempDir config_root("config_hex_case");
    cvf105::TempDir output_root("output_hex_case");
    const cvf105::Json config =
        cvf105::base_config_json("uvc", "", "1A2B", "0C3D", "MixedCase Name");
    cvf105::write_text(config_root.path() / "cvforwin.json", config.dump(2));

    const core::Result<recipes::GlobalConfig> loaded =
        recipes::load_global_config(config_root.path(), output_root.path());

    REQUIRE(loaded.has_value());
    CHECK(loaded.value().camera.vendor_id == "1a2b");
    CHECK(loaded.value().camera.product_id == "0c3d");
    CHECK(loaded.value().camera.friendly_name == "MixedCase Name");
}

TEST_CASE("CVF-105 B2: an uppercase-configured selector resolves against a lowercase descriptor",
          "[cvf-105][B2][identity]")
{
    cvf105::TempDir config_root("config_pipeline_upper");
    cvf105::TempDir output_root("output_pipeline_upper");
    const cvf105::Json config = cvf105::base_config_json("uvc", "", "1A2B", "0C3D", "");
    cvf105::write_text(config_root.path() / "cvforwin.json", config.dump(2));

    core::Result<camera::CameraSelector> loaded =
        loaded_selector(config_root.path(), output_root.path());
    REQUIRE(loaded.has_value());

    const std::vector<camera::CameraDescriptor> candidates{
        descriptor("usb/vid_1a2b&pid_0c3d/probe", "1a2b", "0c3d", "Acme Cam")};

    const core::Result<camera::CameraDescriptor> resolved =
        camera::resolve_identity(candidates, loaded.value());

    REQUIRE(resolved.has_value());
    CHECK(resolved.value().vendor_id == "1a2b");
    CHECK(resolved.value().product_id == "0c3d");
}

TEST_CASE("CVF-105 B2 boundary: mixed-case configured VID/PID resolves through the pipeline",
          "[cvf-105][B2][identity][boundary]")
{
    cvf105::TempDir config_root("config_pipeline_mixed");
    cvf105::TempDir output_root("output_pipeline_mixed");
    const cvf105::Json config = cvf105::base_config_json("uvc", "", "aB01", "Ff02", "");
    cvf105::write_text(config_root.path() / "cvforwin.json", config.dump(2));

    core::Result<camera::CameraSelector> loaded =
        loaded_selector(config_root.path(), output_root.path());
    REQUIRE(loaded.has_value());

    const std::vector<camera::CameraDescriptor> candidates{
        descriptor("usb/probe", "ab01", "ff02", "Acme Cam")};

    const core::Result<camera::CameraDescriptor> resolved =
        camera::resolve_identity(candidates, loaded.value());

    REQUIRE(resolved.has_value());
}

TEST_CASE("CVF-105 B2: lowercase selector VID/PID resolves against a lowercase descriptor",
          "[cvf-105][B2][identity]")
{
    const std::vector<camera::CameraDescriptor> candidates{
        descriptor("usb/vid_1a2b&pid_0c3d/probe", "1a2b", "0c3d", "Acme Cam")};

    const core::Result<camera::CameraDescriptor> resolved =
        camera::resolve_identity(candidates, selector("", "1a2b", "0c3d", ""));

    REQUIRE(resolved.has_value());
    CHECK(resolved.value().product_id == "0c3d");
}

/* ------------------------------------------------------------------------- */
/* B2 contract: device_path and friendly_name stay exact byte comparisons.    */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B2: an exact device_path resolves and a case-different device_path does not",
          "[cvf-105][B2][identity]")
{
    const std::vector<camera::CameraDescriptor> candidates{
        descriptor("Path/Alpha", "1a2b", "0c3d", "Acme Cam"),
        descriptor("Path/Beta", "1a2b", "0c3d", "Acme Cam")};

    const core::Result<camera::CameraDescriptor> exact =
        camera::resolve_identity(candidates, selector("Path/Alpha", "1a2b", "0c3d", ""));
    REQUIRE(exact.has_value());
    CHECK(exact.value().device_path == "Path/Alpha");

    const core::Result<camera::CameraDescriptor> different_case =
        camera::resolve_identity(candidates, selector("path/alpha", "1a2b", "0c3d", ""));
    REQUIRE_FALSE(different_case.has_value());
    CHECK(different_case.failure().code == core::ErrorCode::camera_not_found);
}

TEST_CASE("CVF-105 B2: an exact friendly_name resolves and a case-different one does not",
          "[cvf-105][B2][identity]")
{
    const std::vector<camera::CameraDescriptor> candidates{
        descriptor("Path/Alpha", "1a2b", "0c3d", "MixedCase Name"),
        descriptor("Path/Alpha", "1a2b", "0c3d", "other name")};

    const core::Result<camera::CameraDescriptor> exact =
        camera::resolve_identity(candidates, selector("", "1a2b", "0c3d", "MixedCase Name"));
    REQUIRE(exact.has_value());
    CHECK(exact.value().friendly_name == "MixedCase Name");

    const core::Result<camera::CameraDescriptor> different_case =
        camera::resolve_identity(candidates, selector("", "1a2b", "0c3d", "mixedcase name"));
    REQUIRE_FALSE(different_case.has_value());
    CHECK(different_case.failure().code == core::ErrorCode::camera_not_found);
}

/* ------------------------------------------------------------------------- */
/* B2 negative: canonical duplicates are ambiguous; invalid hex never matches. */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B2 negative: two descriptors with the same canonical identity are ambiguous",
          "[cvf-105][B2][identity][negative]")
{
    const std::vector<camera::CameraDescriptor> candidates{
        descriptor("usb/a", "1a2b", "0c3d", "Acme Cam A"),
        descriptor("usb/b", "1a2b", "0c3d", "Acme Cam B")};

    const core::Result<camera::CameraDescriptor> resolved =
        camera::resolve_identity(candidates, selector("", "1a2b", "0c3d", ""));

    REQUIRE_FALSE(resolved.has_value());
    CHECK(resolved.failure().code == core::ErrorCode::camera_identity_ambiguous);
}

TEST_CASE("CVF-105 B2 negative: a non-hexadecimal or wrong-length selector never matches",
          "[cvf-105][B2][identity][negative]")
{
    const std::vector<camera::CameraDescriptor> candidates{
        descriptor("usb/probe", "1a2b", "0c3d", "Acme Cam")};

    for (const char* vendor : {"1a2g", "1a2", "1a2b3", "ZZZZ", "0x1a"}) {
        const core::Result<camera::CameraDescriptor> resolved =
            camera::resolve_identity(candidates, selector("", vendor, "0c3d", ""));
        INFO("vendor: " << vendor);
        REQUIRE_FALSE(resolved.has_value());
        CHECK(resolved.failure().code == core::ErrorCode::camera_not_found);
    }
}

TEST_CASE("CVF-105 B2 negative: an empty selector is rejected",
          "[cvf-105][B2][identity][negative]")
{
    const std::vector<camera::CameraDescriptor> candidates{
        descriptor("usb/probe", "1a2b", "0c3d", "Acme Cam")};

    const core::Result<camera::CameraDescriptor> resolved =
        camera::resolve_identity(candidates, selector("", "", "", ""));

    REQUIRE_FALSE(resolved.has_value());
}

/* ------------------------------------------------------------------------- */
/* B2 acceptance through configuration load: exact names are preserved.       */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B2: config load keeps device_path and friendly_name byte-exact",
          "[cvf-105][B2][config]")
{
    cvf105::TempDir config_root("config_exact_names");
    cvf105::TempDir output_root("output_exact_names");
    const cvf105::Json config =
        cvf105::base_config_json("uvc", "/dev/CVF-Case Probe", "1A2B", "0C3D", "MixedCase Name");
    cvf105::write_text(config_root.path() / "cvforwin.json", config.dump(2));

    const core::Result<recipes::GlobalConfig> loaded =
        recipes::load_global_config(config_root.path(), output_root.path());

    REQUIRE(loaded.has_value());
    CHECK(loaded.value().camera.device_path == "/dev/CVF-Case Probe");
    CHECK(loaded.value().camera.friendly_name == "MixedCase Name");
    CHECK(loaded.value().camera.vendor_id == "1a2b");
    CHECK(loaded.value().camera.product_id == "0c3d");
}

TEST_CASE("CVF-105 B2: config load leaves already-lowercase VID/PID unchanged",
          "[cvf-105][B2][config]")
{
    cvf105::TempDir config_root("config_lower_case");
    cvf105::TempDir output_root("output_lower_case");
    const cvf105::Json config = cvf105::base_config_json("uvc", "", "1a2b", "0c3d", "");
    cvf105::write_text(config_root.path() / "cvforwin.json", config.dump(2));

    const core::Result<recipes::GlobalConfig> loaded =
        recipes::load_global_config(config_root.path(), output_root.path());

    REQUIRE(loaded.has_value());
    CHECK(loaded.value().camera.vendor_id == "1a2b");
    CHECK(loaded.value().camera.product_id == "0c3d");
}

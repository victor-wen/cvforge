// CVF-004 independent black-box tests: strict global configuration (brief B1, B2).
#include <cstdint>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "cvf004_test_support.h"

using namespace cvf004;

TEST_CASE("CVF-004 B1: a valid cvforwin.json parses to the documented values and records both roots",
          "[cvf-004][B1][config]")
{
    TempDir config_root("config_ok");
    TempDir output_root("output_ok");
    write_config(config_root.path(), valid_config());

    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

    REQUIRE(loaded.has_value());
    const recipes::GlobalConfig& config = loaded.value();
    check_integer(config.schema_version, 1, "schema_version");
    check_text(config.camera.backend, "uvc", "camera.backend");
    check_text(config.camera.device_path, "/dev/cvf004-camera", "camera.device_path");
    check_text(config.camera.vendor_id, "1A2B", "camera.vendor_id");
    check_text(config.camera.product_id, "0C3D", "camera.product_id");
    check_text(config.camera.friendly_name, "CVF-004 probe camera", "camera.friendly_name");
    check_integer(config.base_capture.width, 1920, "base_capture.width");
    check_integer(config.base_capture.height, 1080, "base_capture.height");
    check_number(config.base_capture.frame_rate, 30.0, "base_capture.frame_rate");
    check_text(config.base_capture.pixel_format, "bgr8", "base_capture.pixel_format");
    check_text(config.logging.level, "info", "logging.level");
    check_integer(config.logging.max_file_bytes, 10485760, "logging.max_file_bytes");
    check_integer(config.logging.max_files, 5, "logging.max_files");
    check_integer(config.retention.max_age_days, 30, "retention.max_age_days");
    check_integer(config.retention.max_total_bytes, 10737418240LL, "retention.max_total_bytes");
    CHECK(config.config_root == config_root.path());
    CHECK(config.output_root == output_root.path());
}

TEST_CASE("CVF-004 B1 boundary: every documented pixel_format and level token loads",
          "[cvf-004][B1][config][boundary]")
{
    for (const char* pixel_format : {"any", "mono8", "bgr8", "rgb8"}) {
        for (const char* level : {"trace", "debug", "info", "warn", "error", "critical"}) {
            TempDir config_root("config_tokens");
            TempDir output_root("output_tokens");
            Json config = valid_config();
            config["base_capture"]["pixel_format"] = pixel_format;
            config["logging"]["level"] = level;
            write_config(config_root.path(), config);

            auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

            INFO("pixel_format: " << pixel_format << ", level: " << level);
            REQUIRE(loaded.has_value());
            check_text(loaded.value().base_capture.pixel_format, pixel_format,
                       "base_capture.pixel_format");
            check_text(loaded.value().logging.level, level, "logging.level");
        }
    }
}

TEST_CASE("CVF-004 B2: the camera selector accepts device_path or a VID/PID pair",
          "[cvf-004][B2][config]")
{
    const auto load = [](const Json& config) {
        TempDir config_root("config_selector");
        TempDir output_root("output_selector");
        write_config(config_root.path(), config);
        return recipes::load_global_config(config_root.path(), output_root.path());
    };

    SECTION("device_path alone selects")
    {
        Json config = valid_config();
        config["camera"]["vendor_id"] = "";
        config["camera"]["product_id"] = "";
        auto loaded = load(config);
        REQUIRE(loaded.has_value());
        check_text(loaded.value().camera.device_path, "/dev/cvf004-camera", "camera.device_path");
    }

    SECTION("the VID/PID pair alone selects")
    {
        Json config = valid_config();
        config["camera"]["device_path"] = "";
        auto loaded = load(config);
        REQUIRE(loaded.has_value());
        check_text(loaded.value().camera.vendor_id, "1A2B", "camera.vendor_id");
    }

    SECTION("lowercase hexadecimal VID/PID values are accepted")
    {
        Json config = valid_config();
        config["camera"]["device_path"] = "";
        config["camera"]["vendor_id"] = "1a2b";
        config["camera"]["product_id"] = "0c3d";
        auto loaded = load(config);
        REQUIRE(loaded.has_value());
        check_text(loaded.value().camera.product_id, "0c3d", "camera.product_id");
    }
}

TEST_CASE("CVF-004 B2 negative: a missing cvforwin.json reports config_file_missing",
          "[cvf-004][B2][config][negative]")
{
    TempDir config_root("config_missing");
    TempDir output_root("output_missing");

    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

    check_failure(loaded, core::Status::config_error, core::ErrorCode::config_file_missing);
}

TEST_CASE("CVF-004 B2 negative: malformed JSON reports config_parse_error",
          "[cvf-004][B2][config][negative]")
{
    TempDir config_root("config_malformed");
    TempDir output_root("output_malformed");
    write_text(config_root.path() / "cvforwin.json", R"({"schema_version": 1, "camera": {)");

    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

    check_failure(loaded, core::Status::config_error, core::ErrorCode::config_parse_error);
}

TEST_CASE("CVF-004 B2 negative: duplicate keys are rejected at top level and nested levels",
          "[cvf-004][B2][config][negative]")
{
    SECTION("top-level duplicate")
    {
        const std::string text = replace_once(valid_config_json_text(), R"("schema_version": 1,)",
                                              R"("schema_version": 1, "schema_version": 1,)");
        expect_config_text_failure(text, core::Status::config_error,
                                   core::ErrorCode::config_duplicate_key);
    }

    SECTION("nested camera duplicate")
    {
        const std::string text =
            replace_once(valid_config_json_text(), R"("backend": "uvc",)",
                         R"("backend": "uvc", "backend": "uvc",)");
        expect_config_text_failure(text, core::Status::config_error,
                                   core::ErrorCode::config_duplicate_key);
    }

    SECTION("nested logging duplicate")
    {
        const std::string text = replace_once(valid_config_json_text(), R"("max_files": 5)",
                                              R"("max_files": 5, "max_files": 5)");
        expect_config_text_failure(text, core::Status::config_error,
                                   core::ErrorCode::config_duplicate_key);
    }
}

TEST_CASE("CVF-004 B2 negative: unknown keys are rejected at every documented level",
          "[cvf-004][B2][config][negative]")
{
    SECTION("top level")
    {
        const std::string text = replace_once(valid_config_json_text(), R"("schema_version": 1,)",
                                              R"("schema_version": 1, "extra": 1,)");
        expect_config_text_failure(text, core::Status::config_error,
                                   core::ErrorCode::config_unknown_key);
    }

    SECTION("camera")
    {
        const std::string text = replace_once(valid_config_json_text(), R"("backend": "uvc",)",
                                              R"("backend": "uvc", "zoom": 1,)");
        expect_config_text_failure(text, core::Status::config_error,
                                   core::ErrorCode::config_unknown_key);
    }

    SECTION("logging")
    {
        const std::string text = replace_once(valid_config_json_text(), R"("level": "info",)",
                                              R"("level": "info", "file": 1,)");
        expect_config_text_failure(text, core::Status::config_error,
                                   core::ErrorCode::config_unknown_key);
    }

    SECTION("retention")
    {
        const std::string text = replace_once(valid_config_json_text(), R"("max_age_days": 30,)",
                                              R"("max_age_days": 30, "days": 1,)");
        expect_config_text_failure(text, core::Status::config_error,
                                   core::ErrorCode::config_unknown_key);
    }

    SECTION("base_capture")
    {
        const std::string text = replace_once(valid_config_json_text(), R"("width": 1920,)",
                                              R"("width": 1920, "depth": 8,)");
        expect_config_text_failure(text, core::Status::config_error,
                                   core::ErrorCode::config_unknown_key);
    }
}

TEST_CASE("CVF-004 B2 negative: schema_version values other than 1 are rejected",
          "[cvf-004][B2][config][negative]")
{
    SECTION("schema_version 0")
    {
        Json config = valid_config();
        config["schema_version"] = 0;
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_schema_version);
    }

    SECTION("schema_version 2")
    {
        Json config = valid_config();
        config["schema_version"] = 2;
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_schema_version);
    }
}

TEST_CASE("CVF-004 B2 negative: an unsupported backend is rejected",
          "[cvf-004][B2][config][negative]")
{
    for (const char* backend : {"v4l2", "UVC", ""}) {
        Json config = valid_config();
        config["camera"]["backend"] = backend;
        INFO("backend: " << backend);
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_value_invalid);
    }

#if defined(CVFORWIN_TEST_BACKENDS_ENABLED) && CVFORWIN_TEST_BACKENDS_ENABLED
    // CVF-006: test-enabled builds additionally accept the deterministic test
    // backends. The config keeps the otherwise-valid selector from valid_config()
    // so only the backend value under test varies.
    for (const char* backend : {"file", "synthetic"}) {
        TempDir config_root("config_test_backend");
        TempDir output_root("output_test_backend");
        Json config = valid_config();
        config["camera"]["backend"] = backend;
        write_config(config_root.path(), config);

        INFO("backend: " << backend);
        auto loaded = recipes::load_global_config(config_root.path(), output_root.path());
        REQUIRE(loaded.has_value());
        check_text(loaded.value().camera.backend, backend, "camera.backend");
    }
#endif
}

TEST_CASE("CVF-004 B2 negative: an insufficient camera selector is rejected",
          "[cvf-004][B2][config][negative]")
{
    SECTION("no device_path and no VID/PID")
    {
        Json config = valid_config();
        config["camera"]["device_path"] = "";
        config["camera"]["vendor_id"] = "";
        config["camera"]["product_id"] = "";
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_value_invalid);
    }

    SECTION("vendor_id without product_id")
    {
        Json config = valid_config();
        config["camera"]["device_path"] = "";
        config["camera"]["product_id"] = "";
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_value_invalid);
    }

    SECTION("product_id without vendor_id")
    {
        Json config = valid_config();
        config["camera"]["device_path"] = "";
        config["camera"]["vendor_id"] = "";
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_value_invalid);
    }
}

TEST_CASE("CVF-004 B2 negative: VID/PID values must be exactly four hexadecimal characters",
          "[cvf-004][B2][config][negative]")
{
    for (const char* value : {"1A2", "1A2B3", "1A2G", "1A2B-", " 1A2B", "0x1A"}) {
        Json config = valid_config();
        config["camera"]["device_path"] = "";
        config["camera"]["vendor_id"] = value;
        INFO("vendor_id: [" << value << "]");
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_value_invalid);
    }
}

TEST_CASE("CVF-004 B2 boundary: numeric limits are inclusive at minimum and maximum",
          "[cvf-004][B2][config][boundary]")
{
    SECTION("minimum bounds load and are preserved")
    {
        Json config = valid_config();
        config["base_capture"]["width"] = 1;
        config["base_capture"]["height"] = 1;
        config["base_capture"]["frame_rate"] = 0.5;
        config["logging"]["max_file_bytes"] = 1024;
        config["logging"]["max_files"] = 1;
        config["retention"]["max_age_days"] = 1;
        config["retention"]["max_total_bytes"] = 1048576;
        TempDir config_root("config_min");
        TempDir output_root("output_min");
        write_config(config_root.path(), config);

        auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

        REQUIRE(loaded.has_value());
        check_integer(loaded.value().base_capture.width, 1, "width");
        check_number(loaded.value().base_capture.frame_rate, 0.5, "frame_rate");
        check_integer(loaded.value().logging.max_file_bytes, 1024, "max_file_bytes");
        check_integer(loaded.value().logging.max_files, 1, "max_files");
        check_integer(loaded.value().retention.max_age_days, 1, "max_age_days");
        check_integer(loaded.value().retention.max_total_bytes, 1048576, "max_total_bytes");
    }

    SECTION("maximum bounds load and are preserved")
    {
        Json config = valid_config();
        config["base_capture"]["width"] = 16384;
        config["base_capture"]["height"] = 16384;
        config["base_capture"]["frame_rate"] = 1000.0;
        config["logging"]["max_file_bytes"] = 1073741824;
        config["logging"]["max_files"] = 1000;
        config["retention"]["max_age_days"] = 3650;
        config["retention"]["max_total_bytes"] = 1099511627776LL;
        TempDir config_root("config_max");
        TempDir output_root("output_max");
        write_config(config_root.path(), config);

        auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

        REQUIRE(loaded.has_value());
        check_integer(loaded.value().base_capture.width, 16384, "width");
        check_integer(loaded.value().base_capture.height, 16384, "height");
        check_number(loaded.value().base_capture.frame_rate, 1000.0, "frame_rate");
        check_integer(loaded.value().logging.max_file_bytes, 1073741824, "max_file_bytes");
        check_integer(loaded.value().logging.max_files, 1000, "max_files");
        check_integer(loaded.value().retention.max_age_days, 3650, "max_age_days");
        check_integer(loaded.value().retention.max_total_bytes, 1099511627776LL,
                      "max_total_bytes");
    }
}

TEST_CASE("CVF-004 B2 boundary: a frame_rate just above zero is accepted",
          "[cvf-004][B2][config][boundary]")
{
    // Verify-phase requirement-derived addition: the brief's boundary list names
    // "frame_rate just above 0", so the open lower bound is probed precisely.
    Json config = valid_config();
    config["base_capture"]["frame_rate"] = 0.001;
    TempDir config_root("config_fps_epsilon");
    TempDir output_root("output_fps_epsilon");
    write_config(config_root.path(), config);

    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

    REQUIRE(loaded.has_value());
    check_number(loaded.value().base_capture.frame_rate, 0.001, "frame_rate just above zero");
}

TEST_CASE("CVF-004 B2 negative: out-of-range numeric values are rejected",
          "[cvf-004][B2][config][negative]")
{
    const auto reject = [](const Json& config) {
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_value_invalid);
    };

    {  // width below minimum
        Json config = valid_config();
        config["base_capture"]["width"] = 0;
        reject(config);
    }
    {  // width above maximum
        Json config = valid_config();
        config["base_capture"]["width"] = 16385;
        reject(config);
    }
    {  // height below minimum
        Json config = valid_config();
        config["base_capture"]["height"] = 0;
        reject(config);
    }
    {  // height above maximum
        Json config = valid_config();
        config["base_capture"]["height"] = 16385;
        reject(config);
    }
    {  // frame_rate must be greater than zero
        Json config = valid_config();
        config["base_capture"]["frame_rate"] = 0;
        reject(config);
    }
    {  // frame_rate must not be negative
        Json config = valid_config();
        config["base_capture"]["frame_rate"] = -1.0;
        reject(config);
    }
    {  // frame_rate must not exceed 1000
        Json config = valid_config();
        config["base_capture"]["frame_rate"] = 1000.5;
        reject(config);
    }
    {  // max_file_bytes below minimum
        Json config = valid_config();
        config["logging"]["max_file_bytes"] = 1023;
        reject(config);
    }
    {  // max_file_bytes above maximum
        Json config = valid_config();
        config["logging"]["max_file_bytes"] = 1073741825;
        reject(config);
    }
    {  // max_files below minimum
        Json config = valid_config();
        config["logging"]["max_files"] = 0;
        reject(config);
    }
    {  // max_files above maximum
        Json config = valid_config();
        config["logging"]["max_files"] = 1001;
        reject(config);
    }
    {  // max_age_days below minimum
        Json config = valid_config();
        config["retention"]["max_age_days"] = 0;
        reject(config);
    }
    {  // max_age_days above maximum
        Json config = valid_config();
        config["retention"]["max_age_days"] = 3651;
        reject(config);
    }
    {  // max_total_bytes below minimum
        Json config = valid_config();
        config["retention"]["max_total_bytes"] = 1048575;
        reject(config);
    }
    {  // max_total_bytes above maximum
        Json config = valid_config();
        config["retention"]["max_total_bytes"] = 1099511627777LL;
        reject(config);
    }
}

TEST_CASE("CVF-004 B2 negative: invalid token and type values are rejected",
          "[cvf-004][B2][config][negative]")
{
    const auto reject = [](const Json& config) {
        expect_config_failure(config, core::Status::config_error,
                              core::ErrorCode::config_value_invalid);
    };

    {  // unknown pixel_format token
        Json config = valid_config();
        config["base_capture"]["pixel_format"] = "yuv420";
        reject(config);
    }
    {  // unknown logging level token
        Json config = valid_config();
        config["logging"]["level"] = "verbose";
        reject(config);
    }
    {  // width as a string
        Json config = valid_config();
        config["base_capture"]["width"] = "1920";
        reject(config);
    }
    {  // max_files as a string
        Json config = valid_config();
        config["logging"]["max_files"] = "5";
        reject(config);
    }
    {  // frame_rate as a string
        Json config = valid_config();
        config["base_capture"]["frame_rate"] = "30";
        reject(config);
    }
    {  // camera as an array where an object is required
        Json config = valid_config();
        config["camera"] = Json::array();
        reject(config);
    }
    {  // logging as a number where an object is required
        Json config = valid_config();
        config["logging"] = 5;
        reject(config);
    }
    {  // retention as a string where an object is required
        Json config = valid_config();
        config["retention"] = "none";
        reject(config);
    }
}

TEST_CASE("CVF-004 B2 negative: invalid UTF-8 in a string value is rejected",
          "[cvf-004][B2][config][negative]")
{
    TempDir config_root("config_utf8");
    TempDir output_root("output_utf8");
    const std::string text = replace_once(valid_config_json_text(), "CVF-004 probe camera",
                                          "CVF-"
                                          "\xC3\x28"
                                          "-probe");
    write_text(config_root.path() / "cvforwin.json", text);

    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.failure().status == core::Status::config_error);
    // The brief requires rejection; nlohmann rejects invalid UTF-8 during parsing,
    // so either the parse or the value classification is acceptable here.
    CHECK((loaded.failure().code == core::ErrorCode::config_parse_error ||
           loaded.failure().code == core::ErrorCode::config_value_invalid));
}

TEST_CASE("CVF-004 B2 negative: relative roots are rejected as path_not_absolute",
          "[cvf-004][B2][config][negative]")
{
    SECTION("relative config_root")
    {
        TempDir output_root("output_rel_cfg");
        auto loaded = recipes::load_global_config(std::filesystem::path{"cvf004-relative-config-root"},
                                                  output_root.path());
        check_failure(loaded, core::Status::invalid_argument,
                      core::ErrorCode::path_not_absolute);
    }

    SECTION("relative output_root")
    {
        TempDir config_root("config_rel_out");
        write_config(config_root.path(), valid_config());
        auto loaded = recipes::load_global_config(
            config_root.path(), std::filesystem::path{"cvf004-relative-output-root"});
        check_failure(loaded, core::Status::invalid_argument,
                      core::ErrorCode::path_not_absolute);
    }
}

#ifndef _WIN32
TEST_CASE("CVF-004 B2 negative: an unreadable config file reports config_io_error",
          "[cvf-004][B2][config][negative]")
{
    if (::geteuid() == 0) {
        SKIP("permission bits are not enforced for the superuser");
    }

    TempDir config_root("config_unreadable");
    TempDir output_root("output_unreadable");
    const auto config_file = config_root.path() / "cvforwin.json";
    write_config(config_root.path(), valid_config());
    std::filesystem::permissions(config_file, std::filesystem::perms::none);

    auto loaded = recipes::load_global_config(config_root.path(), output_root.path());

    check_failure(loaded, core::Status::config_error, core::ErrorCode::config_io_error);

    std::filesystem::permissions(config_file, std::filesystem::perms::owner_all);
}
#endif

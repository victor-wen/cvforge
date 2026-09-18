// CVF-105 independent black-box test support.
//
// Shared helpers for the CVF-105 suite only. Header-only and deterministic;
// it includes no production .cpp and no implementation detail. The helpers
// build temporary config roots/recipes at run time, construct exact-length JSON
// payloads for the payload-bound cases, and initialize public C ABI structs.
//
// Owned by the test-engineer; hash-pinned in .ai/test-ownership.yaml.
//
// The dotted file name keeps this auxiliary test header inside the repository's
// test-authoring path policy while remaining a normal header under tests/unit.

#ifndef CVFORWIN_TESTS_UNIT_CVF105_TEST_HELPERS_H_
#define CVFORWIN_TESTS_UNIT_CVF105_TEST_HELPERS_H_

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

namespace cvf105 {

using Json = nlohmann::json;

/* ------------------------------------------------------------------------- */
/* Temporary directories and files.                                           */
/* ------------------------------------------------------------------------- */

class TempDir {
public:
    explicit TempDir(std::string_view label)
    {
        static std::atomic<std::uint64_t> counter{0};
        const auto stamp = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::uint64_t unique = stamp ^ (counter.fetch_add(1) * 0x9E3779B97F4A7C15ull);
        path_ = std::filesystem::temp_directory_path() /
                ("cvf105_" + std::string(label) + "_" + std::to_string(unique));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        REQUIRE(std::filesystem::is_directory(path_));
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

inline void write_text(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
}

/* ------------------------------------------------------------------------- */
/* Config/recipe fixtures written at run time (deterministic, no checked-in   */
/* binary state). The schema is the frozen global_configuration_v1 and        */
/* inspection_recipe_v1 published by the contract.                           */
/* ------------------------------------------------------------------------- */

inline Json base_config_json(std::string_view backend, std::string_view device_path,
                             std::string_view vendor_id, std::string_view product_id,
                             std::string_view friendly_name)
{
    Json config = Json::object();
    config["schema_version"] = 1;
    config["camera"] = Json::object();
    config["camera"]["backend"] = std::string(backend);
    config["camera"]["device_path"] = std::string(device_path);
    config["camera"]["vendor_id"] = std::string(vendor_id);
    config["camera"]["product_id"] = std::string(product_id);
    config["camera"]["friendly_name"] = std::string(friendly_name);
    config["base_capture"] = Json::object();
    config["base_capture"]["width"] = 16;
    config["base_capture"]["height"] = 12;
    config["base_capture"]["frame_rate"] = 30.0;
    config["base_capture"]["pixel_format"] = "bgr8";
    config["logging"] = Json::object();
    config["logging"]["level"] = "info";
    config["logging"]["max_file_bytes"] = 1048576;
    config["logging"]["max_files"] = 2;
    config["retention"] = Json::object();
    config["retention"]["max_age_days"] = 30;
    config["retention"]["max_total_bytes"] = 1073741824;
    return config;
}

inline Json base_recipe_json(std::string_view recipe_id = "example")
{
    Json recipe = Json::object();
    recipe["schema_version"] = 1;
    recipe["recipe_id"] = std::string(recipe_id);
    recipe["algorithm"] = "example.threshold";
    recipe["parameters"] = Json::object();
    recipe["parameters"]["threshold"] = 128;
    recipe["parameters"]["min_pass_ratio"] = 0.0;
    recipe["capture"] = Json::object();
    recipe["capture"]["width"] = 16;
    recipe["capture"]["height"] = 12;
    recipe["capture"]["frame_rate"] = 15.0;
    recipe["capture"]["pixel_format"] = "bgr8";
    recipe["capture"]["settle_frames"] = 0;
    recipe["artifacts"] = Json::object();
    recipe["artifacts"]["save_policy"] = "never";
    recipe["artifacts"]["required"] = false;
    return recipe;
}

/* Writes <root>/cvforwin.json and <root>/recipes/<recipe_id>.json. */
inline void write_config_root(const std::filesystem::path& root, const Json& config,
                              std::string_view recipe_id = "example")
{
    write_text(root / "cvforwin.json", config.dump(2));
    write_text(root / "recipes" / (std::string(recipe_id) + ".json"),
               base_recipe_json(recipe_id).dump(2));
}

/* A config that the deterministic synthetic backend can open (test-enabled
 * builds only). */
inline Json synthetic_config_json()
{
    return base_config_json("synthetic", "synthetic0", "0000", "0000", "Synthetic camera");
}

/* ------------------------------------------------------------------------- */
/* Exact-length JSON payloads for the final-result bound cases.               */
/*                                                                            */
/* {"p":"<pad>"} serializes to exactly 8 + pad bytes, so an object whose       */
/* canonical serialization is a requested byte length is constructed directly. */
/* ------------------------------------------------------------------------- */

inline Json json_object_with_dump_length(std::size_t target_bytes)
{
    REQUIRE(target_bytes >= 8u);
    Json payload = Json::object();
    payload["p"] = std::string(target_bytes - 8u, 'a');
    REQUIRE(payload.dump().size() == target_bytes);
    return payload;
}

inline Json object_out_of_key_order()
{
    Json payload = Json::object();
    payload["z"] = 1;
    payload["a"] = "first";
    payload["m"] = Json::array({1, 2, 3});
    return payload;
}

/* {"s":"<value>"} serializes to exactly 8 + value-byte-count bytes for a value
 * made of valid UTF-8 code points (nlohmann::json::dump() emits UTF-8 bytes
 * verbatim by default). The value is whole repeats of code_point plus an ASCII
 * remainder, so the object's canonical dump is exactly target_bytes. */
inline Json json_string_value_with_dump_length(std::size_t target_bytes,
                                               std::string_view code_point)
{
    REQUIRE(target_bytes >= 8u);
    REQUIRE(!code_point.empty());
    const std::size_t content_bytes = target_bytes - 8u;
    std::string value;
    value.reserve(content_bytes);
    const std::size_t whole = content_bytes / code_point.size();
    for (std::size_t index = 0; index < whole; ++index) {
        value.append(code_point);
    }
    value.append(content_bytes % code_point.size(), 'a');
    Json payload = Json::object();
    payload["s"] = std::move(value);
    REQUIRE(payload.dump().size() == target_bytes);
    return payload;
}

inline Json measurements_and_defects_payload()
{
    Json payload = Json::object();
    payload["measurements"] = Json::object();
    payload["measurements"]["score"] = 0.875;
    payload["measurements"]["x"] = 4;
    payload["measurements"]["y"] = 7;
    payload["measurements"]["width"] = 3;
    payload["measurements"]["height"] = 3;
    payload["defects"] = Json::array();
    payload["defects"].push_back(Json::object());
    payload["defects"][0]["kind"] = "template_mismatch";
    payload["defects"][0]["severity"] = "fail";
    return payload;
}

}  // namespace cvf105

#endif  // CVFORWIN_TESTS_UNIT_CVF105_TEST_HELPERS_H_

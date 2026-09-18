// CVF-105 independent black-box tests: the single final result payload bound.
//
// Brief B6/B7: the complete serialized object is exactly the internal object
// (no truncation, no reordering); the allowed payload bound is 65535 UTF-8
// bytes and one byte more fails deterministically with buffer_too_small before
// the copy; the fixed 65536-byte public buffer always reserves one byte for NUL.
//
// Production surface used (frozen header only): src/runtime/context.h
// k_max_result_payload_bytes and core::Result<std::string>
// serialize_result_payload(const nlohmann::json&); no production .cpp is read.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <string>

#include "cvf105_test.helpers.h"

#include "core/error.h"
#include "core/result.h"
#include "core/status.h"
#include "cvforwin/cvf_api.h"
#include "runtime/context.h"

namespace {

namespace core = cvforwin::core;
namespace rt = cvforwin::runtime;

using cvf105::Json;

bool documented_over_bound_failure(const core::Failure& failure)
{
    return failure.status == core::Status::buffer_too_small ||
           failure.code == core::ErrorCode::runtime_result_too_large;
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* Contract constants.                                                        */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B6 contract: the payload bound is 65535 and the public buffer is 65536",
          "[cvf-105][B6][payload][contract]")
{
    CHECK(rt::k_max_result_payload_bytes == 65535u);
    CHECK(CVF_RESULT_JSON_REQUIRED_CAPACITY == 65536u);
    CHECK(rt::k_max_result_payload_bytes + 1u == CVF_RESULT_JSON_REQUIRED_CAPACITY);
}

/* ------------------------------------------------------------------------- */
/* B7: the emitted JSON equals the internal object exactly.                   */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B7: an empty measurements object serializes to the empty JSON object",
          "[cvf-105][B7][payload]")
{
    const Json payload = Json::object();

    const core::Result<std::string> serialized = rt::serialize_result_payload(payload);

    REQUIRE(serialized.has_value());
    CHECK(serialized.value() == payload.dump());
    CHECK(serialized.value() == "{}");
    CHECK(Json::parse(serialized.value()) == payload);
}

TEST_CASE("CVF-105 B7: a measurements-plus-defects object round-trips without truncation or "
          "reordering",
          "[cvf-105][B7][payload]")
{
    const Json payload = cvf105::measurements_and_defects_payload();

    const core::Result<std::string> serialized = rt::serialize_result_payload(payload);

    REQUIRE(serialized.has_value());
    CHECK(serialized.value() == payload.dump());
    CHECK(serialized.value().size() == payload.dump().size());
    CHECK(Json::parse(serialized.value()) == payload);
    CHECK(serialized.value().find("measurements") != std::string::npos);
    CHECK(serialized.value().find("defects") != std::string::npos);
    CHECK(serialized.value().find("template_mismatch") != std::string::npos);
}

TEST_CASE("CVF-105 B7: key order is canonical and no key is lost",
          "[cvf-105][B7][payload]")
{
    const Json payload = cvf105::object_out_of_key_order();

    const core::Result<std::string> serialized = rt::serialize_result_payload(payload);

    REQUIRE(serialized.has_value());
    CHECK(serialized.value() == payload.dump());
    CHECK(Json::parse(serialized.value()) == payload);
    CHECK(Json::parse(serialized.value()).size() == payload.size());
}

TEST_CASE("CVF-105 B7: multibyte UTF-8 content is copied byte-exactly",
          "[cvf-105][B7][payload]")
{
    Json payload = Json::object();
    payload["label"] = "caf\xC3\xA9 \xF0\x9F\x98\x80";
    payload["measurements"] = Json::object();
    payload["measurements"]["note"] = "\xE6\xB5\x8B\xE8\xAF\x95";

    const core::Result<std::string> serialized = rt::serialize_result_payload(payload);

    REQUIRE(serialized.has_value());
    CHECK(serialized.value() == payload.dump());
    CHECK(serialized.value().size() == payload.dump().size());
    CHECK(Json::parse(serialized.value()) == payload);
}

/* ------------------------------------------------------------------------- */
/* B6 boundary: bound - 1, bound, bound + 1.                                  */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B6 boundary: a payload one byte below the bound succeeds",
          "[cvf-105][B6][payload][boundary]")
{
    const std::size_t target = rt::k_max_result_payload_bytes - 1u;
    const Json payload = cvf105::json_object_with_dump_length(target);

    const core::Result<std::string> serialized = rt::serialize_result_payload(payload);

    REQUIRE(serialized.has_value());
    CHECK(serialized.value().size() == target);
    CHECK(serialized.value() == payload.dump());
    CHECK(Json::parse(serialized.value()) == payload);
}

TEST_CASE("CVF-105 B6 boundary: a payload exactly at the bound succeeds and fits before NUL",
          "[cvf-105][B6][payload][boundary]")
{
    const std::size_t target = rt::k_max_result_payload_bytes;
    const Json payload = cvf105::json_object_with_dump_length(target);

    const core::Result<std::string> serialized = rt::serialize_result_payload(payload);

    REQUIRE(serialized.has_value());
    CHECK(serialized.value().size() == target);
    CHECK(serialized.value().size() == CVF_RESULT_JSON_REQUIRED_CAPACITY - 1u);
    CHECK(serialized.value() == payload.dump());
    CHECK(Json::parse(serialized.value()) == payload);

    // The documented public copy: the caller's 65536-byte buffer always has
    // room for the trailing NUL at the exact bound.
    std::array<char, CVF_RESULT_JSON_REQUIRED_CAPACITY> buffer{};
    std::memcpy(buffer.data(), serialized.value().data(), serialized.value().size());
    buffer[serialized.value().size()] = '\0';
    CHECK(buffer[serialized.value().size()] == '\0');
    CHECK(std::string(buffer.data()) == serialized.value());
}

TEST_CASE("CVF-105 B6 boundary: a payload one byte over the bound fails deterministically before "
          "the copy",
          "[cvf-105][B6][payload][boundary][negative]")
{
    const std::size_t target = rt::k_max_result_payload_bytes + 1u;
    const Json payload = cvf105::json_object_with_dump_length(target);
    REQUIRE(payload.dump().size() == 65536u);

    for (int attempt = 0; attempt < 3; ++attempt) {
        const core::Result<std::string> serialized = rt::serialize_result_payload(payload);

        INFO("attempt: " << attempt);
        REQUIRE_FALSE(serialized.has_value());
        const core::Failure& failure = serialized.failure();
        CHECK(documented_over_bound_failure(failure));
        CHECK(failure.status != core::Status::ok);
    }
}

TEST_CASE("CVF-105 B6 negative: the over-bound failure produces no truncated value",
          "[cvf-105][B6][payload][negative]")
{
    const Json payload = cvf105::json_object_with_dump_length(rt::k_max_result_payload_bytes + 64u);

    const core::Result<std::string> serialized = rt::serialize_result_payload(payload);

    REQUIRE_FALSE(serialized.has_value());
    CHECK(documented_over_bound_failure(serialized.failure()));
}

/* ------------------------------------------------------------------------- */
/* B6 boundary: the bound is a UTF-8 byte count, not a character count.        */
/*                                                                            */
/* A multi-byte payload with one quarter as many characters as bytes is       */
/* accepted exactly at the byte bound, and a payload that is over the byte    */
/* bound fails even though it has far fewer characters than 65535.            */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-105 B6 boundary: a multibyte UTF-8 payload is bounded by bytes, not characters",
          "[cvf-105][B6][payload][boundary]")
{
    // Build the byte-exact payload from a run of three-byte UTF-8 code points
    // (nlohmann::json::dump() emits valid UTF-8 bytes verbatim by default) so
    // the target is a byte length, not a character count.
    constexpr std::string_view kThreeByteCodePoint = "\xE6\xB5\x8B";  // U+6D4B

    const Json exactly_at_bound =
        cvf105::json_string_value_with_dump_length(rt::k_max_result_payload_bytes, kThreeByteCodePoint);
    REQUIRE(exactly_at_bound.dump().size() == rt::k_max_result_payload_bytes);
    // The byte bound is much larger than the character count of the value.
    REQUIRE(exactly_at_bound["s"].get<std::string>().size() < exactly_at_bound.dump().size());

    const core::Result<std::string> at_bound = rt::serialize_result_payload(exactly_at_bound);
    REQUIRE(at_bound.has_value());
    CHECK(at_bound.value().size() == rt::k_max_result_payload_bytes);
    CHECK(at_bound.value() == exactly_at_bound.dump());
    CHECK(Json::parse(at_bound.value()) == exactly_at_bound);

    // One byte over the byte bound fails, even though the payload still holds
    // far fewer characters than the bound.
    const Json one_over_bound =
        cvf105::json_string_value_with_dump_length(rt::k_max_result_payload_bytes + 1u, kThreeByteCodePoint);
    REQUIRE(one_over_bound.dump().size() == rt::k_max_result_payload_bytes + 1u);

    const core::Result<std::string> over_bound = rt::serialize_result_payload(one_over_bound);
    REQUIRE_FALSE(over_bound.has_value());
    CHECK(documented_over_bound_failure(over_bound.failure()));
}

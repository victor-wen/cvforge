// CVF-005 independent black-box tests: CaptureStore::create validation, managed
// root creation, PNG persistence, filename sanitization, and save failure
// (brief B7, B8, B9).

#include "cvf005_test_support.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace cvf005;

TEST_CASE("CVF-005 B7 negative: a relative captures root is rejected as path_not_absolute",
          "[cvf-005][B7][artifacts][negative]")
{
    auto created = art::CaptureStore::create(std::filesystem::path{"cvf005-relative-captures"});

    check_failure(created, core::Status::invalid_argument, core::ErrorCode::path_not_absolute);
}

TEST_CASE("CVF-005 B7: a missing absolute root is created", "[cvf-005][B7][artifacts]")
{
    TempDir base("capture_create");
    const auto root = base.path() / "captures";
    REQUIRE_FALSE(std::filesystem::exists(root));

    std::unique_ptr<art::CaptureStore> store = create_store(root);

    CHECK(std::filesystem::is_directory(root));
}

TEST_CASE("CVF-005 B7 negative: a root that cannot be created reports artifacts_root_error",
          "[cvf-005][B7][artifacts][negative]")
{
    TempDir base("capture_create_fail");
    const auto blocker = base.path() / "blocker";
    write_bytes(blocker, 16u);
    const auto root = blocker / "captures";

    auto created = art::CaptureStore::create(root);

    check_failure(created, core::Status::config_error, core::ErrorCode::artifacts_root_error);
}

TEST_CASE("CVF-005 B8: save writes a decodable PNG directly inside the managed root",
          "[cvf-005][B8][artifacts]")
{
    TempDir base("capture_save");
    const auto root = base.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = create_store(root);
    const cv::Mat frame = bgr_frame(5, 3, 64);

    SaveRequestHolder holder = save_request(frame, "recipe.alpha", "request.beta", 0u);
    auto saved = store->save(holder.request());

    REQUIRE(saved.has_value());
    const std::filesystem::path file{saved.value()};
    check_direct_child(root, file);
    CHECK(file.extension() == ".png");
    CHECK(file.filename().string().ends_with("_0.png"));
    CHECK(file.filename().string().find("recipe.alpha") != std::string::npos);
    CHECK(file.filename().string().find("request.beta") != std::string::npos);
    REQUIRE(std::filesystem::is_regular_file(file));
    check_png(file, 5, 3);
}

TEST_CASE("CVF-005 B8 negative: path-like identifiers cannot escape the managed root",
          "[cvf-005][B8][artifacts][negative]")
{
    TempDir base("capture_hostile");
    const auto root = base.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = create_store(root);
    const cv::Mat frame = bgr_frame(2, 2);

    const std::vector<std::string> hostile = {
        "../escape", "..", "a/b", "a\\b", "with space", "../../etc/passwd", "..\\..\\windows"};

    std::uint64_t sequence = 0;
    for (const auto& recipe_id : hostile) {
        INFO("recipe_id: " << recipe_id);
        SaveRequestHolder holder = save_request(frame, recipe_id, "request", sequence);
        auto saved = store->save(holder.request());
        REQUIRE(saved.has_value());
        const std::filesystem::path file{saved.value()};
        check_direct_child(root, file);
        const std::string name = file.filename().string();
        CHECK(name.find('/') == std::string::npos);
        CHECK(name.find('\\') == std::string::npos);
        CHECK(name.find(' ') == std::string::npos);
        CHECK(std::filesystem::is_regular_file(file));
        ++sequence;
    }

    CHECK(count_files_with_extension(root, ".png") == hostile.size());
    CHECK(count_files_with_extension(base.path(), ".png") == 0u);
}

TEST_CASE("CVF-005 B8 boundary: empty identifiers become unnamed",
          "[cvf-005][B8][artifacts][boundary]")
{
    TempDir base("capture_unnamed");
    const auto root = base.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = create_store(root);
    const cv::Mat frame = bgr_frame(2, 2);

    SECTION("empty recipe_id")
    {
        SaveRequestHolder holder = save_request(frame, "", "request.beta", 0u);
        auto saved = store->save(holder.request());
        REQUIRE(saved.has_value());
        const std::string name{std::filesystem::path{saved.value()}.filename().string()};
        CHECK(name.find("_unnamed_") != std::string::npos);
        CHECK(name.find("request.beta") != std::string::npos);
    }

    SECTION("empty request_id")
    {
        SaveRequestHolder holder = save_request(frame, "recipe.alpha", "", 0u);
        auto saved = store->save(holder.request());
        REQUIRE(saved.has_value());
        const std::string name{std::filesystem::path{saved.value()}.filename().string()};
        CHECK(name.find("_unnamed_") != std::string::npos);
        CHECK(name.find("recipe.alpha") != std::string::npos);
    }

    SECTION("both identifiers empty")
    {
        SaveRequestHolder holder = save_request(frame, "", "", 0u);
        auto saved = store->save(holder.request());
        REQUIRE(saved.has_value());
        const std::string name{std::filesystem::path{saved.value()}.filename().string()};
        // Frozen grammar: <utcstamp>_<recipe>_<request>_<sequence>.png with the
        // fields joined by single underscores, so the both-empty tail is exactly
        // "_unnamed_unnamed_0.png"; a double underscore is not producible.
        CHECK(name.ends_with("_unnamed_unnamed_0.png"));
        CHECK(name.find("_unnamed__unnamed_") == std::string::npos);
    }
}

TEST_CASE("CVF-005 B8 boundary: identifiers longer than 64 bytes are truncated",
          "[cvf-005][B8][artifacts][boundary]")
{
    TempDir base("capture_truncate");
    const auto root = base.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = create_store(root);
    const cv::Mat frame = bgr_frame(2, 2);

    SECTION("100-byte identifiers keep at most the first 64 bytes")
    {
        SaveRequestHolder holder =
            save_request(frame, std::string(100u, 'x'), std::string(100u, 'y'), 0u);
        auto saved = store->save(holder.request());
        REQUIRE(saved.has_value());
        const std::string name{std::filesystem::path{saved.value()}.filename().string()};
        CHECK(name.find(std::string(64u, 'x')) != std::string::npos);
        CHECK(name.find(std::string(65u, 'x')) == std::string::npos);
        CHECK(name.find(std::string(64u, 'y')) != std::string::npos);
        CHECK(name.find(std::string(65u, 'y')) == std::string::npos);
    }

    SECTION("exactly 64 allowed bytes are kept")
    {
        const std::string recipe(64u, 'z');
        SaveRequestHolder holder = save_request(frame, recipe, "request", 0u);
        auto saved = store->save(holder.request());
        REQUIRE(saved.has_value());
        const std::string name{std::filesystem::path{saved.value()}.filename().string()};
        CHECK(name.find(recipe) != std::string::npos);
    }
}

TEST_CASE("CVF-005 B8: two saves produce two distinct files", "[cvf-005][B8][artifacts]")
{
    TempDir base("capture_distinct");
    const auto root = base.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = create_store(root);
    const cv::Mat frame = bgr_frame(2, 2);

    SaveRequestHolder first_holder = save_request(frame, "recipe.alpha", "request.beta", 0u);
    auto first = store->save(first_holder.request());
    SaveRequestHolder second_holder = save_request(frame, "recipe.alpha", "request.beta", 1u);
    auto second = store->save(second_holder.request());

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    const std::filesystem::path first_path{first.value()};
    const std::filesystem::path second_path{second.value()};
    CHECK(first_path != second_path);
    CHECK(first_path.filename().string().ends_with("_0.png"));
    CHECK(second_path.filename().string().ends_with("_1.png"));
    CHECK(std::filesystem::is_regular_file(first_path));
    CHECK(std::filesystem::is_regular_file(second_path));
    CHECK(count_files_with_extension(root, ".png") == 2u);
}

TEST_CASE("CVF-005 B9 negative: an empty frame reports image_encode_error",
          "[cvf-005][B9][artifacts][negative]")
{
    TempDir base("capture_empty_frame");
    const auto root = base.path() / "captures";
    std::unique_ptr<art::CaptureStore> store = create_store(root);
    const cv::Mat empty;

    SaveRequestHolder holder = save_request(empty, "recipe.alpha", "request.beta", 0u);
    auto saved = store->save(holder.request());

    check_failure(saved, core::Status::internal_error, core::ErrorCode::image_encode_error);
    CHECK(count_files_with_extension(root, ".png") == 0u);
}

/*
 * Developer-owned CVF-108 release-naming regression tests.
 *
 * CVF-108 moves the product version to 1.1.0 while the public C ABI stays at
 * version 1 with exactly five exports. The independent test owner covers the
 * observable metadata (CVF-108 OB-1..OB-7); these cases are the developer's
 * own pin on the delivery wiring this change touched, so that a later edit
 * cannot quietly hardcode a release name back into the packaging path while
 * the build metadata declares something else:
 *   a. the staged package directory (`cvforwin-@PROJECT_VERSION@`), the package
 *      archive (`${cvf_package_name}.zip`), and the symbols archive
 *      (`cvforwin-${PROJECT_VERSION}-symbols.zip`) all derive from the single
 *      project version declared once in the root CMakeLists.txt;
 *   b. no cmake/CvfPackage* body contains a literal cvforwin-<x.y.z> name, so
 *      hardcoding a release name into the packaging path is caught;
 *   c. every cvforwin-<x.y.z> name in the CI workflow equals that declared
 *      project version;
 *   d. the public export allowlist is exactly the five approved v1 symbols and
 *      the header's ABI version constant stays at 1.
 *
 * The cases read the real files at run time (the build system passes the
 * repository root as CVF108_REPO_ROOT) and never embed a copy of a file or a
 * line number, so a structural edit that reintroduces the defect is detected.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifndef CVF108_REPO_ROOT
#define CVF108_REPO_ROOT ""
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char* k_product_version = "1.1.0";

fs::path repository_root()
{
    const fs::path configured{CVF108_REPO_ROOT};
    std::error_code error;
    if (!configured.empty() && fs::exists(configured, error)) {
        return fs::absolute(configured);
    }

    fs::path here{__FILE__};
    if (here.is_relative()) {
        here = fs::absolute(here, error);
    }
    for (fs::path probe = here.parent_path(); !probe.empty(); probe = probe.parent_path()) {
        if (fs::exists(probe / "CMakeLists.txt", error) && fs::exists(probe / ".ai", error)) {
            return probe;
        }
        if (probe == probe.root_path()) {
            break;
        }
    }
    return here.parent_path();
}

std::string read_file(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    INFO("required file: " << path.string());
    REQUIRE(stream.good());
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

std::string join(const std::vector<std::string>& values)
{
    std::string out;
    for (const std::string& value : values) {
        if (!out.empty()) {
            out += ", ";
        }
        out += value;
    }
    return out;
}

// The single product version declared by the root build definition.
std::string declared_project_version(const std::string& cmake_text)
{
    static const std::regex pattern(
        R"(project\s*\([^)]*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+[0-9A-Za-z.+\-]*))");
    std::smatch match;
    if (std::regex_search(cmake_text, match, pattern)) {
        return match[1].str();
    }
    return {};
}

// A literal cvforwin-<x.y.z> release name. A name derived from a CMake version
// macro (cvforwin-@PROJECT_VERSION@ or cvforwin-${PROJECT_VERSION}) does not
// match, which is exactly the distinction these cases pin.
std::vector<std::string> hardcoded_release_names(const std::string& text)
{
    static const std::regex pattern(R"(cvforwin-([0-9]+\.[0-9]+\.[0-9]+))");
    std::vector<std::string> names;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        names.push_back(it->str());
    }
    return names;
}

std::string trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string to_lower(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::vector<std::string> parse_def_exports(const std::string& text)
{
    std::vector<std::string> names;
    std::istringstream stream(text);
    std::string line;
    bool in_exports = false;
    while (std::getline(stream, line)) {
        const auto comment = line.find(';');
        if (comment != std::string::npos) {
            line = line.substr(0, comment);
        }
        const std::string trimmed = trim(line);
        if (trimmed.empty()) {
            continue;
        }
        if (!in_exports) {
            if (to_lower(trimmed).rfind("exports", 0) == 0) {
                in_exports = true;
            }
            continue;
        }
        std::istringstream tokens(trimmed);
        std::string token;
        tokens >> token;
        const auto ordinal = token.find('@');
        if (ordinal != std::string::npos) {
            token = token.substr(0, ordinal);
        }
        if (!token.empty()) {
            names.push_back(token);
        }
    }
    return names;
}

std::vector<std::string> expected_exports()
{
    return {
        "cvf_get_abi_version",
        "cvf_initialize",
        "cvf_inspect",
        "cvf_reload_recipes",
        "cvf_shutdown",
    };
}

std::vector<fs::path> packaging_bodies(const fs::path& root)
{
    std::vector<fs::path> modules;
    std::error_code error;
    const fs::path directory = root / "cmake";
    if (!fs::exists(directory, error)) {
        return modules;
    }
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
        if (!entry.is_regular_file(error)) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind("CvfPackage", 0) == 0) {
            modules.push_back(entry.path());
        }
    }
    std::sort(modules.begin(), modules.end());
    return modules;
}

}  // namespace

TEST_CASE("CVF-108 developer: package directory, package archive and symbols archive derive from the declared project version",
          "[cvf108][cvf108-developer][packaging]")
{
    const fs::path root = repository_root();
    const std::string declared = declared_project_version(read_file(root / "CMakeLists.txt"));
    INFO("declared project version: [" << declared << "]");
    CHECK(declared == k_product_version);

    const std::string package_body = read_file(root / "cmake" / "CvfPackage.cmake.in");
    CHECK(package_body.find("set(cvf_package_name \"cvforwin-@PROJECT_VERSION@\")") != std::string::npos);
    CHECK(package_body.find("set(package_root \"${CVF_PACKAGE_DIR}/${cvf_package_name}\")") != std::string::npos);
    CHECK(package_body.find("set(archive \"${CVF_PACKAGE_DIR}/${cvf_package_name}.zip\")") != std::string::npos);

    const std::string symbols_body = read_file(root / "cmake" / "CvfPackage.cmake");
    CHECK(symbols_body.find("cvforwin-${PROJECT_VERSION}-symbols.zip") != std::string::npos);
}

TEST_CASE("CVF-108 developer: no packaging body hardcodes a release name",
          "[cvf108][cvf108-developer][packaging]")
{
    const fs::path root = repository_root();
    const std::string declared = declared_project_version(read_file(root / "CMakeLists.txt"));
    REQUIRE_FALSE(declared.empty());

    const std::vector<fs::path> bodies = packaging_bodies(root);
    REQUIRE_FALSE(bodies.empty());
    for (const fs::path& body : bodies) {
        const std::vector<std::string> hardcoded = hardcoded_release_names(read_file(body));
        INFO(body.filename().string() << " hardcoded release names: [" << join(hardcoded) << "]");
        CHECK(hardcoded.empty());
    }
}

TEST_CASE("CVF-108 developer: every CI cvforwin name matches the declared project version",
          "[cvf108][cvf108-developer][ci]")
{
    const fs::path root = repository_root();
    const std::string declared = declared_project_version(read_file(root / "CMakeLists.txt"));
    REQUIRE(declared == k_product_version);

    const std::vector<std::string> names =
        hardcoded_release_names(read_file(root / ".github" / "workflows" / "windows.yml"));
    REQUIRE_FALSE(names.empty());
    const std::string expected = std::string("cvforwin-") + declared;
    for (const std::string& name : names) {
        INFO("CI release name: " << name);
        CHECK(name == expected);
    }
}

TEST_CASE("CVF-108 developer: the public export allowlist is exactly five symbols and the ABI stays 1",
          "[cvf108][cvf108-developer][abi]")
{
    const fs::path root = repository_root();

    std::vector<std::string> names = parse_def_exports(read_file(root / "src" / "c_api" / "cvforwin.def"));
    std::sort(names.begin(), names.end());
    std::vector<std::string> expected = expected_exports();
    std::sort(expected.begin(), expected.end());

    INFO("exports observed (" << names.size() << "): [" << join(names) << "]");
    CHECK(names.size() == 5u);
    CHECK(names == expected);

    const std::string header = read_file(root / "include" / "cvforwin" / "cvf_api.h");
    CHECK(header.find("#define CVF_ABI_VERSION_V1 1u") != std::string::npos);
}

TEST_CASE("CVF-108 developer boundary: a literal release name is detected while a version macro is not",
          "[cvf108][cvf108-developer][boundary]")
{
    CHECK(hardcoded_release_names("set(cvf_package_name \"cvforwin-1.0.0\")").size() == 1u);
    CHECK(hardcoded_release_names("set(cvf_package_name \"cvforwin-1.1.0\")").size() == 1u);
    CHECK(hardcoded_release_names("set(cvf_package_name \"cvforwin-@PROJECT_VERSION@\")").empty());
    CHECK(hardcoded_release_names("cvforwin-${PROJECT_VERSION}-symbols.zip").empty());

    CHECK(parse_def_exports("EXPORTS\n    a\n    b\n    c\n    d\n    e\n").size() == 5u);
    CHECK(parse_def_exports("EXPORTS\n    a\n    b\n    c\n    d\n").size() == 4u);
    CHECK(parse_def_exports("EXPORTS\n    a\n    b\n    c\n    d\n    e\n    f\n").size() == 6u);
}

// CVF-108 independent black-box test: release 1.1.0 metadata synchronization.
//
// Authority: .ai/test-briefs/CVF-108.yaml and .ai/project-contract.yaml (1.1.0).
//
// This file reads delivery metadata strictly as data -- the top-level build
// definition (CMakeLists.txt), the preset definitions (CMakePresets.json), the
// packaging CMake modules (cmake/CvfPackage*), the dependency manifest
// (vcpkg.json), the CI workflow, the documentation set, the license/notice
// material, the public header's version constant, and the export allowlist --
// and asserts the brief's observable consistency properties. It reads no
// production implementation source, no .cpp, and no diff, and derives no
// expectation from any implementation change.
//
// RED is expected before the release metadata is synchronized: the tree still
// presents 1.0.0 as the current product version in the build definition, the
// dependency manifest, the CI artifact names, the documentation, and the
// notice mapping.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef CVF108_REPO_ROOT
#define CVF108_REPO_ROOT ""
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char* kProductVersion = "1.1.0";
constexpr const char* kLegacyVersion = "1.0.0";

/* ------------------------------------------------------------------------- */
/* Repository location and required-file access                              */
/* ------------------------------------------------------------------------- */

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

struct LoadedFile {
    fs::path path;
    bool exists = false;
    std::string text;
};

std::string read_text(const fs::path& path, bool& exists)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        exists = false;
        return {};
    }
    exists = true;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

LoadedFile load(const fs::path& path)
{
    LoadedFile file;
    file.path = path;
    file.text = read_text(path, file.exists);
    return file;
}

std::vector<fs::path> list_files(const fs::path& directory, const std::string& extension)
{
    std::vector<fs::path> files;
    std::error_code error;
    if (!fs::exists(directory, error)) {
        return files;
    }
    for (const auto& entry : fs::recursive_directory_iterator(directory, error)) {
        if (error) {
            break;
        }
        if (!entry.is_regular_file(error)) {
            continue;
        }
        if (entry.path().extension() == extension) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// A required file that is absent is a failure, never a skip: this returns the
// required paths that do not exist so a caller can assert on the failure set.
std::vector<std::string> missing_required_files(const std::vector<fs::path>& paths)
{
    std::vector<std::string> missing;
    for (const auto& path : paths) {
        std::error_code error;
        if (!fs::exists(path, error)) {
            missing.push_back(path.string());
        }
    }
    return missing;
}

std::vector<fs::path> packaging_modules(const fs::path& root)
{
    std::vector<fs::path> modules;
    std::error_code error;
    const fs::path directory = root / "cmake";
    if (!fs::exists(directory, error)) {
        return modules;
    }
    for (const auto& entry : fs::directory_iterator(directory, error)) {
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

/* ------------------------------------------------------------------------- */
/* Small text helpers                                                        */
/* ------------------------------------------------------------------------- */

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
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

std::string join(const std::vector<std::string>& values, const std::string& separator = ", ")
{
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            out += separator;
        }
        out += values[index];
    }
    return out;
}

std::vector<std::string> split_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string current;
    for (const char character : text) {
        if (character == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (character != '\r') {
            current.push_back(character);
        }
    }
    lines.push_back(current);
    return lines;
}

/* ------------------------------------------------------------------------- */
/* Product-version matching                                                  */
/* ------------------------------------------------------------------------- */

bool is_version_boundary_before(char character)
{
    return !(std::isdigit(static_cast<unsigned char>(character)) || character == '.');
}

bool is_version_boundary_after(char character)
{
    if (std::isdigit(static_cast<unsigned char>(character)) || std::isalpha(static_cast<unsigned char>(character))) {
        return false;
    }
    return !(character == '.' || character == '-' || character == '+' || character == '_');
}

// True only when `version` occurs as the complete product-version token. A
// superset such as "1.1.0-something", "1.1.0.0", "1.10.0", "2.0.0", or "1.1"
// must not satisfy an exact "1.1.0" requirement.
bool contains_exact_version(std::string_view text, std::string_view version)
{
    std::size_t position = 0;
    while ((position = text.find(version, position)) != std::string_view::npos) {
        const bool before_ok = position == 0 || is_version_boundary_before(text[position - 1]);
        const std::size_t end = position + version.size();
        const bool after_ok = end >= text.size() || is_version_boundary_after(text[end]);
        if (before_ok && after_ok) {
            return true;
        }
        position = end;
    }
    return false;
}

std::vector<std::string> find_semver_tokens(const std::string& text)
{
    static const std::regex pattern(R"(\d+\.\d+\.\d+)");
    std::vector<std::string> tokens;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        tokens.push_back(it->str());
    }
    return tokens;
}

struct ProductName {
    std::string version;
    std::size_t position = 0;
    std::size_t line = 0;
};

std::size_t line_of(const std::string& text, std::size_t position)
{
    std::size_t line = 0;
    for (std::size_t index = 0; index < position && index < text.size(); ++index) {
        if (text[index] == '\n') {
            ++line;
        }
    }
    return line;
}

std::vector<ProductName> find_cvforwin_versions(const std::string& text)
{
    // Capture a four-part superset such as 1.1.0.0 in full, so it is not
    // silently accepted as the exact three-part product version 1.1.0.
    static const std::regex pattern(R"(cvforwin-(\d+\.\d+\.\d+(?:\.\d+)*))", std::regex::icase);
    std::vector<ProductName> names;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        ProductName name;
        name.version = (*it)[1].str();
        name.position = static_cast<std::size_t>(it->position());
        name.line = line_of(text, name.position);
        names.push_back(name);
    }
    return names;
}

/* ------------------------------------------------------------------------- */
/* Historical-context scoping                                                */
/*                                                                            */
/* A historical reference (a changelog entry, old release notes, archived      */
/* material) is legitimate and must not be reported as an inconsistency. Only  */
/* a reference that presents an earlier version as the current product version */
/* fails. History is recognised only when a historical marker sits in the same */
/* logical unit as the version reference -- the containing sentence, list item, */
/* or table row -- never merely within a fixed line distance. A list item or   */
/* sentence that is the direct body of a version heading owns that heading as  */
/* its changelog-entry unit. A stale current-release mention that happens to   */
/* sit next to a genuine historical mention belongs to a different unit and is */
/* therefore reported.                                                         */
/* ------------------------------------------------------------------------- */

struct LineSpan {
    std::size_t begin = 0;
    std::size_t end = 0;
};

std::vector<LineSpan> line_spans(const std::string& text)
{
    std::vector<LineSpan> spans;
    std::size_t index = 0;
    while (index <= text.size()) {
        const std::size_t begin = index;
        while (index < text.size() && text[index] != '\n') {
            ++index;
        }
        std::size_t end = index;
        if (end > begin && text[end - 1] == '\r') {
            --end;
        }
        spans.push_back({begin, end});
        if (index >= text.size()) {
            break;
        }
        ++index;
    }
    return spans;
}

std::string line_text(const std::string& text, const LineSpan& span)
{
    return text.substr(span.begin, span.end - span.begin);
}

std::size_t line_index_for(const std::vector<LineSpan>& spans, std::size_t position)
{
    std::size_t found = 0;
    for (std::size_t index = 0; index < spans.size(); ++index) {
        if (spans[index].begin <= position) {
            found = index;
        } else {
            break;
        }
    }
    return found;
}

bool is_heading_line(const std::string& line)
{
    const std::string trimmed = trim(line);
    return !trimmed.empty() && trimmed.front() == '#';
}

bool is_version_heading_line(const std::string& line)
{
    static const std::regex version_heading(R"(^\s*#{1,6}\s+v?\d+\.\d+\.\d+\b)");
    return std::regex_search(line, version_heading);
}

bool is_table_row_line(const std::string& line)
{
    const std::string trimmed = trim(line);
    return !trimmed.empty() && trimmed.front() == '|' && trimmed.find('|', 1) != std::string::npos;
}

bool is_list_item_line(const std::string& line)
{
    const std::string trimmed = trim(line);
    if (trimmed.empty()) {
        return false;
    }
    if ((trimmed.front() == '-' || trimmed.front() == '*' || trimmed.front() == '+') &&
        (trimmed.size() == 1 || std::isspace(static_cast<unsigned char>(trimmed[1])) != 0)) {
        return true;
    }
    std::size_t digits = 0;
    while (digits < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[digits])) != 0) {
        ++digits;
    }
    return digits > 0 && digits + 1 < trimmed.size() &&
           (trimmed[digits] == '.' || trimmed[digits] == ')') &&
           std::isspace(static_cast<unsigned char>(trimmed[digits + 1])) != 0;
}

bool is_sentence_terminator(const std::string& line, std::size_t index)
{
    const char character = line[index];
    if (character != '.' && character != '!' && character != '?') {
        return false;
    }
    const char after = index + 1 < line.size() ? line[index + 1] : '\0';
    if (after != '\0' && std::isspace(static_cast<unsigned char>(after)) == 0 && after != '"' && after != ')') {
        return false;
    }
    if (character == '.') {
        const char before = index > 0 ? line[index - 1] : '\0';
        if (std::isdigit(static_cast<unsigned char>(before)) != 0 && std::isdigit(static_cast<unsigned char>(after)) != 0) {
            return false;
        }
    }
    return true;
}

std::pair<std::size_t, std::size_t> sentence_span(const std::string& text, const LineSpan& span, std::size_t position)
{
    const std::string line = line_text(text, span);
    const std::size_t local = position >= span.begin ? position - span.begin : 0;

    std::size_t end = line.size();
    for (std::size_t index = local; index < line.size(); ++index) {
        if (is_sentence_terminator(line, index)) {
            end = index + 1;
            break;
        }
    }
    std::size_t begin = 0;
    for (std::size_t index = local; index > 0; --index) {
        if (is_sentence_terminator(line, index - 1)) {
            begin = index;
            break;
        }
    }
    return {span.begin + begin, span.begin + end};
}

bool unit_is_historical(std::string_view unit)
{
    static const std::vector<std::string> markers = {
        "changelog",
        "release notes",
        "release history",
        "revision history",
        "previous release",
        "prior release",
        "older release",
        "legacy",
        "superseded",
        "deprecated",
        "migration",
        "upgrade from",
        "historical",
        "no longer",
        "previously",
    };
    static const std::regex version_heading(R"(^\s*#{1,6}\s+v?\d+\.\d+\.\d+\b)");

    const std::string lower = to_lower(std::string(unit));
    for (const auto& marker : markers) {
        if (contains(lower, marker)) {
            return true;
        }
    }
    return std::regex_search(std::string(unit), version_heading);
}

// The historical marker must belong to the version reference's own logical
// unit. A genuine history entry and a stale current-release mention that merely
// sit on nearby lines are therefore treated differently.
bool is_historical_context(const std::string& text, std::size_t position)
{
    const std::vector<LineSpan> spans = line_spans(text);
    if (spans.empty()) {
        return false;
    }
    const std::size_t index = line_index_for(spans, position);
    const std::string line = line_text(text, spans[index]);

    std::size_t begin = spans[index].begin;
    std::size_t end = spans[index].end;

    if (is_heading_line(line) || is_table_row_line(line)) {
        // The unit is exactly the heading or the table row.
    } else if (is_list_item_line(line)) {
        // A list item owns its indented continuation lines.
        for (std::size_t probe = index + 1; probe < spans.size(); ++probe) {
            const std::string candidate = line_text(text, spans[probe]);
            if (trim(candidate).empty() || std::isspace(static_cast<unsigned char>(candidate.front())) == 0) {
                break;
            }
            end = spans[probe].end;
        }
    } else {
        const auto sentence = sentence_span(text, spans[index], position);
        begin = sentence.first;
        end = sentence.second;
    }

    // A list item or sentence that is the direct body of a version heading owns
    // that heading as its changelog-entry unit; only blank lines may intervene.
    for (std::size_t probe = index; probe > 0; --probe) {
        const std::string previous = line_text(text, spans[probe - 1]);
        if (trim(previous).empty()) {
            continue;
        }
        if (is_version_heading_line(previous)) {
            begin = std::min(begin, spans[probe - 1].begin);
        }
        break;
    }

    return unit_is_historical(text.substr(begin, end - begin));
}

std::vector<std::string> scan_current_version_mismatches(const std::string& text, std::string_view current)
{
    std::vector<std::string> mismatches;
    for (const auto& name : find_cvforwin_versions(text)) {
        if (name.version == current) {
            continue;
        }
        if (is_historical_context(text, name.position)) {
            continue;
        }
        mismatches.push_back(name.version);
    }
    return mismatches;
}

/* ------------------------------------------------------------------------- */
/* Declaration parsers                                                       */
/* ------------------------------------------------------------------------- */

std::string extract_cmake_project_version(const std::string& text)
{
    static const std::regex pattern(
        R"(project\s*\([^)]*?\bVERSION\s+([0-9]+\.[0-9]+\.[0-9]+[0-9A-Za-z.+\-]*))");
    std::smatch match;
    if (std::regex_search(text, match, pattern)) {
        return match[1].str();
    }
    return {};
}

std::vector<std::string> extract_json_version_fields(const std::string& text)
{
    static const std::regex pattern(R"re("version(?:-string|-semver|-date)?"\s*:\s*"([^"]*)")re");
    std::vector<std::string> versions;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) {
        versions.push_back((*it)[1].str());
    }
    return versions;
}

std::vector<std::pair<std::string, int>> extract_abi_version_macros(const std::string& text)
{
    static const std::regex pattern(R"(^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.*)$)");
    std::vector<std::pair<std::string, int>> macros;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, pattern)) {
            continue;
        }
        const std::string name = match[1].str();
        if (name.find("ABI") == std::string::npos || name.find("VERSION") == std::string::npos) {
            continue;
        }
        std::string value = match[2].str();
        const auto comment = value.find("//");
        if (comment != std::string::npos) {
            value = value.substr(0, comment);
        }
        const auto block_comment = value.find("/*");
        if (block_comment != std::string::npos) {
            value = value.substr(0, block_comment);
        }
        value = trim(value);
        while (!value.empty() && (value.back() == 'u' || value.back() == 'U')) {
            value.pop_back();
        }
        value = trim(value);
        if (value.empty()) {
            continue;
        }
        const bool numeric = std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isdigit(character) != 0;
        });
        if (!numeric) {
            continue;
        }
        macros.emplace_back(name, std::stoi(value));
    }
    return macros;
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
        "cvf_reload_recipes",
        "cvf_inspect",
        "cvf_shutdown",
    };
}

}  // namespace

/* ------------------------------------------------------------------------- */
/* OB-1: build metadata declares product version 1.1.0                       */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-108 OB-1: the top-level build definition declares product version 1.1.0",
          "[cvf-108][OB-1][metadata]")
{
    const LoadedFile file = load(repository_root() / "CMakeLists.txt");
    REQUIRE(file.exists);  // absence of a required file is a failure, never a skip

    const std::string observed = extract_cmake_project_version(file.text);
    INFO("CMakeLists.txt project VERSION observed: [" << observed << "]");
    CHECK(observed == kProductVersion);
    CHECK_FALSE(contains_exact_version(file.text, kLegacyVersion));
}

TEST_CASE("CVF-108 OB-1: the dependency manifest version field matches the product version",
          "[cvf-108][OB-1][metadata]")
{
    const LoadedFile file = load(repository_root() / "vcpkg.json");
    REQUIRE(file.exists);

    const std::vector<std::string> versions = extract_json_version_fields(file.text);
    INFO("vcpkg.json version fields observed: [" << join(versions) << "]");
    REQUIRE_FALSE(versions.empty());
    for (const auto& version : versions) {
        INFO("vcpkg.json version field: [" << version << "]");
        CHECK(version == kProductVersion);
    }
}

TEST_CASE("CVF-108 OB-1/OB-2: package names derive from the single CMake project version",
          "[cvf-108][OB-1][OB-2][metadata]")
{
    const auto modules = packaging_modules(repository_root());
    REQUIRE_FALSE(modules.empty());

    bool references_project_version = false;
    std::vector<std::string> tokens;
    std::vector<std::string> stale;
    for (const auto& module : modules) {
        const LoadedFile file = load(module);
        REQUIRE(file.exists);
        const std::string name = module.filename().string();
        if (file.text.find("PROJECT_VERSION") != std::string::npos) {
            references_project_version = true;
        }
        for (const auto& token : find_semver_tokens(file.text)) {
            tokens.push_back(name + ":" + token);
            if (token != kProductVersion) {
                stale.push_back(name + ":" + token);
            }
        }
        for (const auto& version : find_cvforwin_versions(file.text)) {
            if (version.version != kProductVersion) {
                stale.push_back(name + ":" + version.version);
            }
        }
    }

    INFO("packaging module version tokens: [" << join(tokens) << "]");
    INFO("packaging module stale tokens: [" << join(stale) << "]");
    CHECK(references_project_version);
    CHECK(stale.empty());
}

TEST_CASE("CVF-108 OB-1: preset metadata presents no stale product version",
          "[cvf-108][OB-1][metadata]")
{
    const LoadedFile file = load(repository_root() / "CMakePresets.json");
    REQUIRE(file.exists);

    std::vector<std::string> stale;
    for (const auto& token : find_semver_tokens(file.text)) {
        if (token != kProductVersion) {
            stale.push_back(token);
        }
    }
    INFO("CMakePresets.json three-part tokens: [" << join(stale) << "]");
    CHECK(stale.empty());
    CHECK_FALSE(contains_exact_version(file.text, kLegacyVersion));
}

/* ------------------------------------------------------------------------- */
/* OB-2: package, archive, and CI artifact names                             */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-108 OB-2: CI artifact and package-path names carry product version 1.1.0",
          "[cvf-108][OB-2][metadata]")
{
    const fs::path workflows_dir = repository_root() / ".github" / "workflows";
    std::vector<fs::path> workflows = list_files(workflows_dir, ".yml");
    for (const auto& extra : list_files(workflows_dir, ".yaml")) {
        workflows.push_back(extra);
    }
    REQUIRE_FALSE(workflows.empty());

    bool saw_any = false;
    std::vector<std::string> observed;
    std::vector<std::string> stale;
    for (const auto& workflow : workflows) {
        const LoadedFile file = load(workflow);
        REQUIRE(file.exists);
        for (const auto& name : find_cvforwin_versions(file.text)) {
            saw_any = true;
            const std::string label = workflow.filename().string() + ":" + name.version;
            observed.push_back(label);
            if (name.version != kProductVersion) {
                stale.push_back(label);
            }
        }
    }

    INFO("CI cvforwin-<version> names observed: [" << join(observed) << "]");
    INFO("CI stale names: [" << join(stale) << "]");
    CHECK(saw_any);
    CHECK(stale.empty());

    const LoadedFile windows = load(workflows_dir / "windows.yml");
    REQUIRE(windows.exists);
    const std::string lower = to_lower(windows.text);
    CHECK(contains(lower, "package"));
    CHECK(contains(lower, "symbols"));
}

/* ------------------------------------------------------------------------- */
/* OB-6: documentation                                                       */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-108 OB-6: documentation quotes current 1.1.0 package/artifact names",
          "[cvf-108][OB-6][metadata][docs]")
{
    const auto docs = list_files(repository_root() / "docs", ".md");
    REQUIRE_FALSE(docs.empty());

    bool saw_current = false;
    std::vector<std::string> observed;
    std::vector<std::string> stale;
    for (const auto& doc : docs) {
        const LoadedFile file = load(doc);
        REQUIRE(file.exists);
        for (const auto& name : find_cvforwin_versions(file.text)) {
            const std::string label = doc.filename().string() + ":" + name.version;
            observed.push_back(label);
            if (name.version == kProductVersion) {
                saw_current = true;
            } else if (!is_historical_context(file.text, name.position)) {
                stale.push_back(label);
            }
        }
    }

    INFO("documentation cvforwin-<version> names observed: [" << join(observed) << "]");
    INFO("non-historical stale documentation names: [" << join(stale) << "]");
    CHECK(saw_current);
    CHECK(stale.empty());
}

TEST_CASE("CVF-108 OB-6: documentation describes the template.match example with its asset and recipe",
          "[cvf-108][OB-6][metadata][docs]")
{
    const auto docs = list_files(repository_root() / "docs", ".md");
    REQUIRE_FALSE(docs.empty());

    std::string combined;
    for (const auto& doc : docs) {
        const LoadedFile file = load(doc);
        REQUIRE(file.exists);
        combined += file.text;
        combined += '\n';
    }
    const std::string lower = to_lower(combined);
    CHECK(contains(lower, "template.match"));
    CHECK(contains(lower, "recipe"));
    CHECK(contains(lower, "asset"));
    CHECK(contains(lower, ".png"));
}

TEST_CASE("CVF-108 OB-6: documentation describes background artifact retention",
          "[cvf-108][OB-6][metadata][docs]")
{
    const auto docs = list_files(repository_root() / "docs", ".md");
    REQUIRE_FALSE(docs.empty());

    static const std::vector<std::string> background_markers = {
        "background",
        "asynchron",
        "off the inspection",
        "outside the inspection",
        "never blocks",
        "does not block",
        "without blocking",
    };

    std::vector<std::string> evidence;
    for (const auto& doc : docs) {
        const LoadedFile file = load(doc);
        REQUIRE(file.exists);
        const std::vector<std::string> lines = split_lines(file.text);
        for (std::size_t index = 0; index < lines.size(); ++index) {
            if (!contains(to_lower(lines[index]), "retention")) {
                continue;
            }
            const std::size_t first = index > 4 ? index - 4 : 0;
            const std::size_t last = std::min(index + 4, lines.size() - 1);
            for (std::size_t probe = first; probe <= last; ++probe) {
                const std::string lower = to_lower(lines[probe]);
                for (const auto& marker : background_markers) {
                    if (contains(lower, marker)) {
                        evidence.push_back(doc.filename().string() + ": " + lines[index]);
                        probe = last;
                        break;
                    }
                }
            }
        }
    }

    INFO("background-retention documentation evidence: [" << join(evidence, " | ") << "]");
    CHECK_FALSE(evidence.empty());
}

/* ------------------------------------------------------------------------- */
/* OB-4/OB-5: frozen public ABI surface                                      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-108 OB-4: the public header keeps ABI version 1 and the five v1 entry points",
          "[cvf-108][OB-4][metadata][abi]")
{
    const LoadedFile header = load(repository_root() / "include" / "cvforwin" / "cvf_api.h");
    REQUIRE(header.exists);

    const auto macros = extract_abi_version_macros(header.text);
    std::vector<std::string> observed;
    bool saw_v1 = false;
    for (const auto& macro : macros) {
        observed.push_back(macro.first + "=" + std::to_string(macro.second));
        if (macro.first == "CVF_ABI_VERSION_V1" && macro.second == 1) {
            saw_v1 = true;
        }
    }
    INFO("ABI version macros observed: [" << join(observed) << "]");
    REQUIRE_FALSE(macros.empty());
    for (const auto& macro : macros) {
        INFO("ABI version macro: " << macro.first << " = " << macro.second);
        CHECK(macro.second == 1);
    }
    CHECK(saw_v1);

    for (const auto& name : expected_exports()) {
        INFO("public header entry point: " << name);
        CHECK(contains(header.text, name));
    }
}

TEST_CASE("CVF-108 OB-5: the export allowlist is exactly the five approved v1 symbols",
          "[cvf-108][OB-5][metadata][abi]")
{
    const LoadedFile def = load(repository_root() / "src" / "c_api" / "cvforwin.def");
    REQUIRE(def.exists);

    std::vector<std::string> names = parse_def_exports(def.text);
    std::sort(names.begin(), names.end());
    std::vector<std::string> expected = expected_exports();
    std::sort(expected.begin(), expected.end());

    INFO("exports observed (" << names.size() << "): [" << join(names) << "]");
    INFO("exports expected (" << expected.size() << "): [" << join(expected) << "]");
    CHECK(names.size() == 5u);
    CHECK(names == expected);
}

/* ------------------------------------------------------------------------- */
/* OB-7: license and notice material                                         */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-108 OB-7: notices cover the image codec and name the current release",
          "[cvf-108][OB-7][metadata][licenses]")
{
    const fs::path root = repository_root();
    const auto notices = list_files(root / "LICENSES", ".txt");
    REQUIRE_FALSE(notices.empty());

    const LoadedFile readme = load(root / "LICENSES" / "README.md");
    REQUIRE(readme.exists);
    const std::string lower = to_lower(readme.text);
    CHECK(contains(lower, "opencv"));
    CHECK(contains(lower, "imgcodecs"));
    CHECK(contains(lower, "libpng"));
    CHECK(contains(lower, "libjpeg-turbo"));

    bool opencv_notice = false;
    for (const auto& notice : notices) {
        if (to_lower(notice.filename().string()).find("opencv") != std::string::npos) {
            opencv_notice = true;
        }
    }
    CHECK(opencv_notice);

    static const std::regex phrase(R"(cvforwin\s+([0-9]+\.[0-9]+\.[0-9]+))");
    std::vector<std::string> claimed;
    for (std::sregex_iterator it(readme.text.begin(), readme.text.end(), phrase), end; it != end; ++it) {
        claimed.push_back((*it)[1].str());
    }
    INFO("LICENSES/README.md cvforwin <version> claims: [" << join(claimed) << "]");
    REQUIRE_FALSE(claimed.empty());
    for (const auto& version : claimed) {
        INFO("LICENSES/README.md release claim: [" << version << "]");
        CHECK(version == kProductVersion);
    }
}

/* ------------------------------------------------------------------------- */
/* Boundary and negative demonstrations                                      */
/* ------------------------------------------------------------------------- */

TEST_CASE("CVF-108 boundary: exact 1.1.0 matching rejects supersets and other versions",
          "[cvf-108][boundary][version]")
{
    CHECK(contains_exact_version("1.1.0", kProductVersion));
    CHECK(contains_exact_version("VERSION 1.1.0)", kProductVersion));
    CHECK_FALSE(contains_exact_version("1.1.0-something-else", kProductVersion));
    CHECK_FALSE(contains_exact_version("1.1.0.0", kProductVersion));
    CHECK_FALSE(contains_exact_version("1.10.0", kProductVersion));
    CHECK_FALSE(contains_exact_version("2.0.0", kProductVersion));
    CHECK_FALSE(contains_exact_version("1.1", kProductVersion));
    CHECK_FALSE(contains_exact_version("11.1.0", kProductVersion));
    CHECK_FALSE(contains_exact_version("01.1.0", kProductVersion));
    CHECK_FALSE(contains_exact_version(kLegacyVersion, kProductVersion));
}

TEST_CASE("CVF-108 boundary: historical mentions are not reported as current inconsistencies",
          "[cvf-108][boundary][docs]")
{
    const std::string changelog =
        "# Changelog\n\n"
        "## 1.0.0 - 2025-01-01\n"
        "- initial release; archived artifact build/cvforwin-1.0.0.zip\n";
    CHECK(scan_current_version_mismatches(changelog, kProductVersion).empty());

    const std::string current_doc =
        "# Installation\n\n"
        "Download cvforwin-1.0.0.zip from the release page.\n";
    CHECK(scan_current_version_mismatches(current_doc, kProductVersion).size() == 1u);
}

TEST_CASE("CVF-108 boundary: historical context is scoped to the same logical unit",
          "[cvf-108][boundary][docs]")
{
    // Detection control (b): a genuine historical mention is legitimate and must
    // NOT be reported. The prior-release list item is owned by its version
    // heading, and a sentence that itself names the previous release is exempt.
    const std::string genuine_history =
        "# Release history\n\n"
        "## 1.0.0 - 2025-01-01\n"
        "- prior release; archived build/cvforwin-1.0.0.zip\n";
    CHECK(scan_current_version_mismatches(genuine_history, kProductVersion).empty());

    const std::string genuine_sentence =
        "Download cvforwin-1.0.0.zip from the previous release archive.\n";
    CHECK(scan_current_version_mismatches(genuine_sentence, kProductVersion).empty());

    // Detection control (a): a stale current-release mention that is adjacent to
    // a genuine historical mention but belongs to a different logical unit must
    // be REPORTED, even though the historical mention is only one line away.
    const std::string adjacent_units =
        "# Release history\n\n"
        "## 1.0.0 - 2025-01-01\n"
        "- prior release; archived build/cvforwin-1.0.0.zip\n"
        "Download cvforwin-1.0.0.zip as the current release.\n";
    CHECK(scan_current_version_mismatches(adjacent_units, kProductVersion) ==
          std::vector<std::string>{"1.0.0"});

    // The same rule separates two sentences on a single line.
    const std::string same_line_different_sentence =
        "The 1.0.0 build was historical. Download cvforwin-1.0.0.zip as the current release.\n";
    CHECK(scan_current_version_mismatches(same_line_different_sentence, kProductVersion) ==
          std::vector<std::string>{"1.0.0"});

    // A table row is its own unit: the historical row is exempt while the
    // current row quoting the old version is reported.
    const std::string table =
        "# Changelog\n\n"
        "| Version | Status | Artifact |\n"
        "| --- | --- | --- |\n"
        "| 1.0.0 | prior release | cvforwin-1.0.0.zip |\n"
        "| 1.0.0 | current download | cvforwin-1.0.0.zip |\n";
    CHECK(scan_current_version_mismatches(table, kProductVersion) ==
          std::vector<std::string>{"1.0.0"});
}

TEST_CASE("CVF-108 boundary: the export allowlist parser pins five and rejects four or six",
          "[cvf-108][boundary][abi]")
{
    CHECK(parse_def_exports("EXPORTS\n    a\n    b\n    c\n    d\n    e\n").size() == 5u);
    CHECK(parse_def_exports("EXPORTS\n    a\n    b\n    c\n    d\n").size() == 4u);
    CHECK(parse_def_exports("EXPORTS\n    a\n    b\n    c\n    d\n    e\n    f\n").size() == 6u);
    CHECK(parse_def_exports("; comment\nEXPORTS\n    a @1\n    b @2\n    c\n    d\n    e\n").size() == 5u);
}

TEST_CASE("CVF-108 boundary: the ABI constant parser accepts exactly 1 and rejects 2",
          "[cvf-108][boundary][abi]")
{
    const auto one = extract_abi_version_macros("#define CVF_ABI_VERSION_V1 1u\n");
    REQUIRE(one.size() == 1u);
    CHECK(one.front().second == 1);
    CHECK(one.front().first == "CVF_ABI_VERSION_V1");

    const auto two = extract_abi_version_macros("#define CVF_ABI_VERSION_V1 2\n");
    REQUIRE(two.size() == 1u);
    CHECK(two.front().second == 2);
}

TEST_CASE("CVF-108 boundary: a missing required file is detected as missing, never skipped",
          "[cvf-108][boundary][missing]")
{
    const LoadedFile missing = load(repository_root() / "tests/unit/__cvf108_absent_file__.txt");
    CHECK_FALSE(missing.exists);
    CHECK(missing.text.empty());
}

TEST_CASE("CVF-108 boundary: a required-file set reports the absent entry as a failure, never a skip",
          "[cvf-108][boundary][missing]")
{
    const fs::path root = repository_root();
    const std::vector<fs::path> required = {
        root / "CMakeLists.txt",
        root / "vcpkg.json",
        root / "tests/unit/__cvf108_absent_file__.txt",
    };
    const std::vector<std::string> missing = missing_required_files(required);
    REQUIRE(missing.size() == 1u);
    CHECK(missing.front().find("__cvf108_absent_file__.txt") != std::string::npos);

    std::vector<fs::path> present_only;
    for (const auto& path : required) {
        if (path.filename().string() != "__cvf108_absent_file__.txt") {
            present_only.push_back(path);
        }
    }
    CHECK(missing_required_files(present_only).empty());
}

TEST_CASE("CVF-108 boundary: a cvforwin name with a four-part superset version is not accepted as current",
          "[cvf-108][boundary][version]")
{
    const auto superset = find_cvforwin_versions("path: cvforwin-1.1.0.0.zip\n");
    REQUIRE(superset.size() == 1u);
    CHECK(superset.front().version == "1.1.0.0");
    CHECK(superset.front().version != kProductVersion);

    const auto artifact = find_cvforwin_versions("name: cvforwin-1.1.0-package\n");
    REQUIRE(artifact.size() == 1u);
    CHECK(artifact.front().version == kProductVersion);
}

TEST_CASE("CVF-108 boundary: a partially applied bump is detected in exactly the unsynchronized file",
          "[cvf-108][boundary][version]")
{
    struct MetadataFile {
        std::string name;
        std::string text;
    };

    auto file_problems = [](const MetadataFile& file) {
        const auto ends_with = [](const std::string& value, std::string_view suffix) {
            return value.size() >= suffix.size() &&
                   value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        std::vector<std::string> problems;
        if (file.name == "CMakeLists.txt") {
            if (extract_cmake_project_version(file.text) != std::string(kProductVersion)) {
                problems.push_back("project-version");
            }
        } else if (file.name == "vcpkg.json") {
            for (const auto& version : extract_json_version_fields(file.text)) {
                if (version != kProductVersion) {
                    problems.push_back("manifest-version=" + version);
                }
            }
        } else if (ends_with(file.name, ".yml")) {
            for (const auto& name : find_cvforwin_versions(file.text)) {
                if (name.version != kProductVersion) {
                    problems.push_back("ci-name=" + name.version);
                }
            }
        } else if (ends_with(file.name, ".md")) {
            for (const auto& version : scan_current_version_mismatches(file.text, kProductVersion)) {
                problems.push_back("doc-name=" + version);
            }
        }
        return problems;
    };

    auto unsynchronized_files = [&file_problems](const std::vector<MetadataFile>& files) {
        std::vector<std::string> offenders;
        for (const auto& file : files) {
            if (!file_problems(file).empty()) {
                offenders.push_back(file.name);
            }
        }
        return offenders;
    };

    const std::vector<MetadataFile> synchronized = {
        {"CMakeLists.txt", "project(cvforwin VERSION 1.1.0 LANGUAGES C CXX)\n"},
        {"vcpkg.json", "{\n  \"version-string\": \"1.1.0\"\n}\n"},
        {"windows.yml", "name: cvforwin-1.1.0-package\n"},
        {"packaging.md", "Download cvforwin-1.1.0.zip from the release page.\n"},
    };
    CHECK(unsynchronized_files(synchronized).empty());

    // A bump applied to only one of several files is detected, and the report
    // names exactly the file that was not synchronized.
    std::vector<MetadataFile> partial = synchronized;
    partial[1].text = "{\n  \"version-string\": \"1.0.0\"\n}\n";
    CHECK(unsynchronized_files(partial) == std::vector<std::string>{"vcpkg.json"});

    partial = synchronized;
    partial[0].text = "project(cvforwin VERSION 1.0.0 LANGUAGES C CXX)\n";
    CHECK(unsynchronized_files(partial) == std::vector<std::string>{"CMakeLists.txt"});

    partial = synchronized;
    partial[2].text = "name: cvforwin-1.0.0-package\n";
    CHECK(unsynchronized_files(partial) == std::vector<std::string>{"windows.yml"});

    partial = synchronized;
    partial[3].text = "Download cvforwin-1.0.0.zip from the release page.\n";
    CHECK(unsynchronized_files(partial) == std::vector<std::string>{"packaging.md"});

    // A superset of the expected version is not accepted as the current one.
    partial = synchronized;
    partial[1].text = "{\n  \"version-string\": \"1.1.0-beta\"\n}\n";
    CHECK(unsynchronized_files(partial) == std::vector<std::string>{"vcpkg.json"});

    partial = synchronized;
    partial[0].text = "project(cvforwin VERSION 1.1.0.0 LANGUAGES C CXX)\n";
    CHECK(unsynchronized_files(partial) == std::vector<std::string>{"CMakeLists.txt"});
}

TEST_CASE("CVF-108 boundary: an export set with one symbol added, removed, or replaced is rejected",
          "[cvf-108][boundary][abi]")
{
    std::vector<std::string> expected = expected_exports();
    std::sort(expected.begin(), expected.end());

    auto def_text = [](const std::vector<std::string>& names) {
        std::string text = "EXPORTS\n";
        for (const auto& name : names) {
            text += "    " + name + "\n";
        }
        return text;
    };
    auto sorted_exports = [](const std::string& text) {
        std::vector<std::string> names = parse_def_exports(text);
        std::sort(names.begin(), names.end());
        return names;
    };

    // Baseline: the approved set matches exactly.
    CHECK(sorted_exports(def_text(expected)) == expected);

    // One approved symbol removed: four names, a strict subset.
    std::vector<std::string> removed = expected;
    removed.pop_back();
    CHECK(sorted_exports(def_text(removed)).size() == 4u);
    CHECK(sorted_exports(def_text(removed)) != expected);

    // One symbol added: six names, a strict superset.
    std::vector<std::string> added = expected;
    added.push_back("cvf_extra");
    std::sort(added.begin(), added.end());
    CHECK(sorted_exports(def_text(added)).size() == 6u);
    CHECK(sorted_exports(def_text(added)) != expected);

    // One approved symbol replaced: still five names, but not the approved set.
    std::vector<std::string> replaced = expected;
    replaced.front() = "cvf_impostor";
    std::sort(replaced.begin(), replaced.end());
    CHECK(sorted_exports(def_text(replaced)).size() == 5u);
    CHECK(sorted_exports(def_text(replaced)) != expected);
}

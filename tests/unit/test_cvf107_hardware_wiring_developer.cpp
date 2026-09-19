/*
 * Developer-owned CVF-107 build-wiring regression tests.
 *
 * The opt-in Windows UVC hardware suite is not a CTest test and has no
 * production behaviour that the portable suite can execute. What can and must
 * be pinned automatically is its build wiring in CMakeLists.txt, because the
 * CVF-103 blocking defect was exactly of this class: a Windows target
 * registered behind a feature gate that is OFF by default and unset in CI, so
 * no build anywhere ever compiled it. Human review found CVF-103; these cases
 * must make that class of mistake fail automatically.
 *
 * The cases read the real CMakeLists.txt at run time (the build system passes
 * its directory as CVFORWIN_TEST_SOURCE_DIR) and parse the command/block
 * structure; they never embed a copy of the file or a line number, so a
 * structural edit that reintroduces a defect is detected:
 *   a. add_executable(cvf_hw_uvc_smoke ...) lies lexically inside an
 *      if(CVFORWIN_BUILD_HARDWARE_TESTS) block AND inside an if(WIN32) block
 *      nested in that block, with the executable before both matching
 *      endif()s, and its only source is tests/hardware/uvc_smoke.cpp.
 *   b. cvf_hw_uvc_smoke is never registered with CTest: no add_test statement
 *      mentions it and neither catch_discover_tests nor gtest_discover_tests
 *      is applied to it.
 *   c. _CRT_SECURE_NO_WARNINGS is target-scoped and applied to no unexpected
 *      target. cvf_hw_uvc_smoke must carry it; the only other target allowed
 *      to carry it is cvf006_lifecycle, the pre-existing CVF-006 C ABI test
 *      that introduced the same opt-out before CVF-107 (CMakeLists.txt:774).
 *      Any further target acquiring the opt-out fails this case, which is the
 *      leak the scoped definition exists to prevent.
 *   d. cvf_hw_uvc_smoke appears in no file under .github, so it can never join
 *      a hosted no-camera CI job.
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef CVFORWIN_TEST_SOURCE_DIR
#error "CVFORWIN_TEST_SOURCE_DIR must be defined by the build system for this target"
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char* k_hardware_target = "cvf_hw_uvc_smoke";
constexpr const char* k_hardware_source = "tests/hardware/uvc_smoke.cpp";
constexpr const char* k_hardware_gate = "CVFORWIN_BUILD_HARDWARE_TESTS";
constexpr const char* k_windows_gate = "WIN32";
constexpr const char* k_crt_definition = "_CRT_SECURE_NO_WARNINGS";

// One parsed CMake command: the lower-cased command name, the raw text between
// its parentheses, and the 0-based line on which the name occurs.
struct Command {
    std::string name;
    std::string args;
    std::size_t line;
};

// One if()/endif() pair. close is npos when the file is unbalanced.
struct IfBlock {
    std::size_t open;
    std::size_t close = std::string::npos;
    std::string condition;
};

std::string to_lower(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool is_identifier_char(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
}

// True when token appears in text bounded by non-identifier characters, so
// "WIN32" does not match "WIN32_EXTRA".
bool contains_identifier(const std::string& text, const std::string& token)
{
    std::size_t position = text.find(token);
    while (position != std::string::npos) {
        const bool left_ok = position == 0 || !is_identifier_char(text[position - 1]);
        const std::size_t after = position + token.size();
        const bool right_ok = after >= text.size() || !is_identifier_char(text[after]);
        if (left_ok && right_ok) {
            return true;
        }
        position = text.find(token, position + 1);
    }
    return false;
}

// Catch2 v3.4 in this toolchain has no CHECK_MESSAGE/REQUIRE_MESSAGE; these
// helpers attach the invariant description to the assertion instead. A failing
// REQUIRE propagates Catch::TestFailureException out of the helper and aborts
// the test case exactly like an inline REQUIRE.
void check_message(bool condition, const std::string& message)
{
    INFO(message);
    CHECK(condition);
}

void require_message(bool condition, const std::string& message)
{
    INFO(message);
    REQUIRE(condition);
}

// Parse command name followed by a balanced-parenthesis argument list. '#' line
// comments are skipped; identifier characters before a name are respected so
// elseif()/endif() do not look like if().
std::vector<Command> tokenize(const std::string& text)
{
    std::vector<Command> commands;
    std::size_t index = 0;
    std::size_t line = 0;
    const std::size_t size = text.size();
    while (index < size) {
        const char ch = text[index];
        if (ch == '\n') {
            ++line;
            ++index;
            continue;
        }
        if (ch == '#') {
            while (index < size && text[index] != '\n') {
                ++index;
            }
            continue;
        }
        if (!std::isalpha(static_cast<unsigned char>(ch)) && ch != '_') {
            ++index;
            continue;
        }
        const std::size_t name_start = index;
        while (index < size && is_identifier_char(text[index])) {
            ++index;
        }
        const bool name_is_bounded =
            name_start == 0 || !is_identifier_char(text[name_start - 1]);
        const std::string name = text.substr(name_start, index - name_start);
        std::size_t open = index;
        while (open < size && (text[open] == ' ' || text[open] == '\t')) {
            ++open;
        }
        if (!name_is_bounded || open >= size || text[open] != '(') {
            continue;
        }
        const std::size_t args_start = open + 1;
        const std::size_t command_line = line;
        std::size_t cursor = args_start;
        int depth = 1;
        while (cursor < size && depth > 0) {
            if (text[cursor] == '\n') {
                ++line;
            } else if (text[cursor] == '(') {
                ++depth;
            } else if (text[cursor] == ')') {
                --depth;
            }
            ++cursor;
        }
        if (depth != 0) {
            break;
        }
        commands.push_back({to_lower(name), text.substr(args_start, cursor - 1 - args_start), command_line});
        index = cursor;
    }
    return commands;
}

std::vector<std::string> split_args(const std::string& args)
{
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (const char ch : args) {
        if (ch == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(ch);
    }
    if (!current.empty()) {
        tokens.push_back(current);
    }
    return tokens;
}

std::vector<IfBlock> collect_if_blocks(const std::vector<Command>& commands)
{
    std::vector<IfBlock> blocks;
    std::vector<std::size_t> stack;
    for (std::size_t index = 0; index < commands.size(); ++index) {
        if (commands[index].name == "if") {
            blocks.push_back({index, std::string::npos, commands[index].args});
            stack.push_back(blocks.size() - 1);
        } else if (commands[index].name == "endif" && !stack.empty()) {
            blocks[stack.back()].close = index;
            stack.pop_back();
        }
    }
    return blocks;
}

bool block_contains(const IfBlock& block, std::size_t command_index)
{
    return block.close != std::string::npos && block.open < command_index && command_index < block.close;
}

bool block_nested_in(const IfBlock& outer, const IfBlock& inner)
{
    return outer.close != std::string::npos && inner.close != std::string::npos && outer.open < inner.open && inner.close < outer.close;
}

std::optional<std::size_t> find_add_executable(const std::vector<Command>& commands, const std::string& target)
{
    for (std::size_t index = 0; index < commands.size(); ++index) {
        if (commands[index].name != "add_executable") {
            continue;
        }
        const std::vector<std::string> tokens = split_args(commands[index].args);
        if (!tokens.empty() && tokens.front() == target) {
            return index;
        }
    }
    return std::nullopt;
}

struct Wiring {
    std::string text;
    std::vector<Command> commands;
    std::vector<IfBlock> blocks;
};

Wiring load_wiring()
{
    const fs::path path = fs::path(CVFORWIN_TEST_SOURCE_DIR) / "CMakeLists.txt";
    require_message(fs::is_regular_file(path), "CMakeLists.txt is missing at " + path.string());
    std::ifstream input(path, std::ios::binary);
    require_message(input.good(), "CMakeLists.txt cannot be opened at " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Wiring wiring;
    wiring.text = buffer.str();
    wiring.commands = tokenize(wiring.text);
    wiring.blocks = collect_if_blocks(wiring.commands);
    return wiring;
}

}  // namespace

TEST_CASE("CVF-107 add_executable(cvf_hw_uvc_smoke) is inside the opt-in Windows gate",
          "[cvf107][build-wiring]")
{
    const Wiring wiring = load_wiring();
    const std::optional<std::size_t> executable = find_add_executable(wiring.commands, k_hardware_target);
    require_message(executable.has_value(),
                    "add_executable(cvf_hw_uvc_smoke ...) is absent from CMakeLists.txt");
    CAPTURE(executable.value());

    SECTION("the executable is inside an if(CVFORWIN_BUILD_HARDWARE_TESTS) block")
    {
        std::optional<IfBlock> hardware_block;
        for (const IfBlock& block : wiring.blocks) {
            if (contains_identifier(block.condition, k_hardware_gate) && block_contains(block, executable.value())) {
                hardware_block = block;
                break;
            }
        }
        check_message(hardware_block.has_value(),
                      "add_executable(cvf_hw_uvc_smoke ...) must be lexically inside "
                      "if(CVFORWIN_BUILD_HARDWARE_TESTS)");
        if (!hardware_block.has_value()) {
            return;
        }
        check_message(executable.value() > hardware_block->open && executable.value() < hardware_block->close,
                      "add_executable(cvf_hw_uvc_smoke ...) must lie between the hardware gate opening and its "
                      "matching endif()");
    }

    SECTION("the executable is inside an if(WIN32) block nested in the hardware gate")
    {
        std::optional<IfBlock> hardware_block;
        for (const IfBlock& block : wiring.blocks) {
            if (contains_identifier(block.condition, k_hardware_gate) && block_contains(block, executable.value())) {
                hardware_block = block;
                break;
            }
        }
        require_message(hardware_block.has_value(), "the CVFORWIN_BUILD_HARDWARE_TESTS block was not found");
        std::optional<IfBlock> windows_block;
        for (const IfBlock& block : wiring.blocks) {
            if (contains_identifier(block.condition, k_windows_gate) && block_contains(block, executable.value()) && block_nested_in(*hardware_block, block)) {
                windows_block = block;
                break;
            }
        }
        check_message(windows_block.has_value(),
                      "add_executable(cvf_hw_uvc_smoke ...) must be lexically inside an if(WIN32) block nested in "
                      "the CVFORWIN_BUILD_HARDWARE_TESTS block");
        if (!windows_block.has_value()) {
            return;
        }
        check_message(executable.value() > windows_block->open && executable.value() < windows_block->close,
                      "add_executable(cvf_hw_uvc_smoke ...) must lie between the WIN32 gate opening and its matching "
                      "endif()");
    }

    SECTION("the hardware suite source path is exactly tests/hardware/uvc_smoke.cpp")
    {
        const std::vector<std::string> tokens = split_args(wiring.commands[executable.value()].args);
        require_message(!tokens.empty() && tokens.front() == k_hardware_target,
                        "the add_executable target must be cvf_hw_uvc_smoke");
        const std::vector<std::string> sources(tokens.begin() + 1, tokens.end());
        check_message(sources.size() == 1 && sources.front() == k_hardware_source,
                      "cvf_hw_uvc_smoke must have exactly one source, tests/hardware/uvc_smoke.cpp");
    }
}

TEST_CASE("CVF-107 cvf_hw_uvc_smoke is never registered with CTest", "[cvf107][build-wiring]")
{
    const Wiring wiring = load_wiring();
    for (const Command& command : wiring.commands) {
        if (command.name == "add_test" && contains_identifier(command.args, k_hardware_target)) {
            check_message(false,
                          "add_test at CMakeLists.txt:" + std::to_string(command.line + 1) + " registers cvf_hw_uvc_smoke with CTest");
        }
        if ((command.name == "catch_discover_tests" || command.name == "gtest_discover_tests") && contains_identifier(command.args, k_hardware_target)) {
            check_message(false,
                          command.name + " at CMakeLists.txt:" + std::to_string(command.line + 1) + " applies discovery to cvf_hw_uvc_smoke");
        }
    }
    const bool parsed_any_add_test =
        std::any_of(wiring.commands.begin(), wiring.commands.end(), [](const Command& command) {
            return command.name == "add_test";
        });
    check_message(parsed_any_add_test, "sanity: at least one add_test statement must be parsed from CMakeLists.txt");
}

TEST_CASE("CVF-107 _CRT_SECURE_NO_WARNINGS is target-scoped and not applied to any unexpected target",
          "[cvf107][build-wiring]")
{
    const Wiring wiring = load_wiring();
    std::vector<std::string> targets_with_definition;
    for (const Command& command : wiring.commands) {
        if (command.name == "add_compile_definitions" || command.name == "add_definitions") {
            check_message(!contains_identifier(command.args, k_crt_definition),
                          "CMakeLists.txt:" + std::to_string(command.line + 1) + " applies _CRT_SECURE_NO_WARNINGS globally instead of per target");
        }
        if (command.name == "target_compile_definitions" && contains_identifier(command.args, k_crt_definition)) {
            const std::vector<std::string> tokens = split_args(command.args);
            if (!tokens.empty()) {
                targets_with_definition.push_back(tokens.front());
            }
        }
    }

    SECTION("cvf_hw_uvc_smoke carries the MSVC CRT opt-out and it is PRIVATE")
    {
        bool hardware_has_definition = false;
        for (const Command& command : wiring.commands) {
            if (command.name != "target_compile_definitions") {
                continue;
            }
            const std::vector<std::string> tokens = split_args(command.args);
            const bool names_hardware = !tokens.empty() && tokens.front() == k_hardware_target;
            if (names_hardware && contains_identifier(command.args, k_crt_definition)) {
                hardware_has_definition = true;
                check_message(std::find(tokens.begin(), tokens.end(), "PRIVATE") != tokens.end(),
                              "the _CRT_SECURE_NO_WARNINGS definition for cvf_hw_uvc_smoke must be PRIVATE");
            }
        }
        check_message(hardware_has_definition,
                      "cvf_hw_uvc_smoke must carry a target-scoped _CRT_SECURE_NO_WARNINGS definition");
    }

    SECTION("no target beyond the documented exceptions carries the opt-out")
    {
        const std::vector<std::string> allowed = {"cvf006_lifecycle", k_hardware_target};
        for (const std::string& target : targets_with_definition) {
            check_message(std::find(allowed.begin(), allowed.end(), target) != allowed.end(),
                          "_CRT_SECURE_NO_WARNINGS is applied to unexpected target '" + target + "'");
        }
        check_message(std::find(targets_with_definition.begin(), targets_with_definition.end(), k_hardware_target) != targets_with_definition.end(),
                      "cvf_hw_uvc_smoke must be among the targets carrying _CRT_SECURE_NO_WARNINGS");
        check_message(targets_with_definition.size() == allowed.size(),
                      "exactly the documented targets may carry _CRT_SECURE_NO_WARNINGS");
    }
}

TEST_CASE("CVF-107 cvf_hw_uvc_smoke never joins a hosted no-camera CI job", "[cvf107][build-wiring]")
{
    const fs::path github = fs::path(CVFORWIN_TEST_SOURCE_DIR) / ".github";
    const bool github_exists = fs::is_directory(github);
    if (!github_exists) {
        // Absence is asserted explicitly instead of letting the file scan pass
        // silently with nothing to inspect.
        check_message(!github_exists,
                      "no .github directory exists, so no hosted CI file can reference cvf_hw_uvc_smoke");
        return;
    }
    check_message(github_exists, ".github exists and is audited for cvf_hw_uvc_smoke references");

    std::vector<fs::path> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(github)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    check_message(!files.empty(), "the .github directory contains no file to audit");
    for (const fs::path& file : files) {
        std::ifstream input(file, std::ios::binary);
        require_message(input.good(), "cannot open CI file " + file.string());
        std::ostringstream buffer;
        buffer << input.rdbuf();
        check_message(!contains_identifier(buffer.str(), k_hardware_target),
                      "CI file " + file.string() + " references cvf_hw_uvc_smoke");
    }
}

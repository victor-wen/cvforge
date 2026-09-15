// CVF-005 independent black-box tests: the save-policy decision primitive
// (brief B10 and negative_cases).

#include "cvf005_test_support.h"

#include <array>
#include <string_view>

using namespace cvf005;

namespace {

struct DecisionCase {
    std::string_view policy;
    core::Verdict verdict;
    bool execution_ok;
    art::SaveDecision expected;
};

constexpr std::array<DecisionCase, 17> kDecisionCases = {
    // never: always skip
    DecisionCase{"never", core::Verdict::pass, true, art::SaveDecision::skip},
    DecisionCase{"never", core::Verdict::fail, true, art::SaveDecision::skip},
    DecisionCase{"never", core::Verdict::not_evaluated, false, art::SaveDecision::skip},
    // always: always save
    DecisionCase{"always", core::Verdict::pass, true, art::SaveDecision::save},
    DecisionCase{"always", core::Verdict::fail, true, art::SaveDecision::save},
    DecisionCase{"always", core::Verdict::not_evaluated, false, art::SaveDecision::save},
    // fail_or_error: save iff the execution failed or the verdict is FAIL
    DecisionCase{"fail_or_error", core::Verdict::pass, true, art::SaveDecision::skip},
    DecisionCase{"fail_or_error", core::Verdict::pass, false, art::SaveDecision::save},
    DecisionCase{"fail_or_error", core::Verdict::fail, true, art::SaveDecision::save},
    DecisionCase{"fail_or_error", core::Verdict::fail, false, art::SaveDecision::save},
    DecisionCase{"fail_or_error", core::Verdict::not_evaluated, true, art::SaveDecision::skip},
    DecisionCase{"fail_or_error", core::Verdict::not_evaluated, false, art::SaveDecision::save},
    // unknown tokens: skip
    DecisionCase{"sometimes", core::Verdict::fail, false, art::SaveDecision::skip},
    DecisionCase{"", core::Verdict::fail, true, art::SaveDecision::skip},
    DecisionCase{"ALWAYS", core::Verdict::fail, true, art::SaveDecision::skip},
    DecisionCase{"fail", core::Verdict::pass, false, art::SaveDecision::skip},
    DecisionCase{"fail_or_error ", core::Verdict::pass, false, art::SaveDecision::skip},
};

}  // namespace

TEST_CASE("CVF-005 B10: decide_capture_save follows the frozen policy table",
          "[cvf-005][B10][artifacts][contract]")
{
    CHECK(art::SaveDecision::skip != art::SaveDecision::save);

    for (const auto& entry : kDecisionCases) {
        INFO("policy: [" << entry.policy << "] verdict: "
                         << core::to_public_verdict(entry.verdict)
                         << " execution_ok: " << entry.execution_ok);
        CHECK(art::decide_capture_save(entry.policy, entry.verdict, entry.execution_ok) ==
              entry.expected);
    }
}

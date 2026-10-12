#include "core/Verdict.hpp"

namespace {

struct VerdictText {
    const char* text;
    bool tiedToTest;
};

VerdictText describe(Verdict verdict) {
    switch (verdict) {
        case Verdict::Accepted:
            return {"Accepted", false};

        case Verdict::WrongAnswer:
            return {"Wrong Answer", true};

        case Verdict::CompilationError:
            return {"Compilation Error", false};

        case Verdict::TimeLimitExceeded:
            return {"Time Limit Exceeded", true};

        case Verdict::MemoryLimitExceeded:
            return {"Memory Limit Exceeded", true};

        case Verdict::RuntimeError:
            return {"Runtime Error", true};

        case Verdict::OutputLimitExceeded:
            return {"Output Limit Exceeded", true};

        case Verdict::SystemError:
            return {"System Error - please resubmit", false};

        case Verdict::NoTestCases:
            return {"There are no test cases", false};

        case Verdict::Rejected:
            return {"Rejected - server busy, please resubmit", false};
    }

    return { // This should never happen if the enum and switch statement are kept in sync.
        "System Error - please resubmit",
        false
    };
}

} // namespace

std::string toString(Verdict verdict) {
    return describe(verdict).text;
}

std::string toString(const JudgeResult& result) {
    const VerdictText description = describe(result.verdict);

    std::string text = description.text;
    if (description.tiedToTest && result.failedTest > 0) {
        text += " on test " + std::to_string(result.failedTest);
    }
    return text;
}

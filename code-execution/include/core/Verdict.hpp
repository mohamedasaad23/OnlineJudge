#pragma once

#include <string>

enum class Verdict {
    Accepted,
    WrongAnswer,
    CompilationError,
    TimeLimitExceeded,
    MemoryLimitExceeded,
    RuntimeError,
    OutputLimitExceeded,
    SystemError,
    NoTestCases,
    Rejected
};

struct JudgeResult {
    Verdict verdict = Verdict::SystemError;
    int failedTest = 0;
    int maxTimeMs = 0;
};

// Converts a Verdict enum value to its corresponding string representation.
std::string toString(Verdict verdict);

// Converts a JudgeResult struct to its corresponding string representation.
std::string toString(const JudgeResult& result);

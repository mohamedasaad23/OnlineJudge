#pragma once

#include "core/Language.hpp"
#include "core/Models.hpp"
#include "core/Verdict.hpp"

#include <string>
#include <vector>

struct JudgeRequest {
    Language language;
    std::string sourceCode;
    std::vector<TestCase> testCases;
    int timeLimitMs;
    int memoryLimitMb;
    std::string workDir;
};

class Judge {
public:
    static JudgeResult grade(const JudgeRequest& request);
};

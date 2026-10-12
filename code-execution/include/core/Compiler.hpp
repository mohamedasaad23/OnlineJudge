#pragma once

#include "core/Language.hpp"

#include <string>

enum class PrepareStatus {
    Ready,
    CompilationError,
    InternalError
};

struct PrepareResult {
    PrepareStatus status = PrepareStatus::InternalError;
    std::string executablePath;
};

struct CompileLimits {
    int timeoutMs = 10000;
    int memoryLimitMb = 512;
};

class Compiler {
public:
    //   C++    -> compiled with g++ into workDir/solution.out
    //   Python -> workDir/solution.py + syntax validation

    static PrepareResult prepare(
        Language language,
        const std::string& sourceCode,
        const std::string& workDir,
        const CompileLimits& limits = CompileLimits{}
    );
};

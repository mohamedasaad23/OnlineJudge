#pragma once

#include <string>

enum class ExecutionStatus {
    Completed,
    TimeLimitExceeded,
    MemoryLimitExceeded,
    RuntimeError,
    OutputLimitExceeded,
    SystemError
};

struct ExecutionResult {
    ExecutionStatus status;
    int timeMs;
    int memoryKb;
};

struct ExecutionRequest {
    std::string workDir;
    std::string executablePath;
    std::string inputPath;
    std::string outputPath;
    int timeLimitMs;
    int memoryLimitMb;
};

class ProcessExecutor {
public:
    static ExecutionResult execute(const ExecutionRequest& request);
};

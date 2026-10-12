#pragma once

#include "core/ProcessExecutor.hpp"

#include <string>

struct RawOutcome {
    bool startupFailed = false;

    bool killedForTime = false;
    bool outputLimitExceeded = false;

    bool exited = false;
    bool signaled = false;

    int exitCode = -1;
    int terminatingSignal = 0;

    int memoryKb = 0;
    int timeMs = 0;

    std::string capturedStderr;

    int timeLimitMs = 0;
    int memoryLimitMb = 0;
};

class OutcomeClassifier {
public:
    static ExecutionStatus classify(const RawOutcome& outcome);
};

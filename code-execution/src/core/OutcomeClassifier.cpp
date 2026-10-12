#include "core/OutcomeClassifier.hpp"

#include <csignal>
#include <sstream>

namespace {

bool indicatesAllocationFailure(const std::string& capturedStderr) {

    // std::bad_alloc is thrown by C++ new when it fails to allocate memory.
    if (capturedStderr.find("std::bad_alloc") != std::string::npos) {
        return true;
    }

    std::istringstream stderrStream(capturedStderr);
    std::string line;

    while (std::getline(stderrStream, line)) {
        const size_t lastNonWhitespace = line.find_last_not_of(" \t\r\f\v");

        if (lastNonWhitespace == std::string::npos) {
            continue;
        }

        line.resize(lastNonWhitespace + 1);

        // Python raises MemoryError when it fails to allocate memory.
        if (line == "MemoryError" || line.rfind("MemoryError:", 0) == 0) {
            return true;
        }
    }

    return false;
}

bool indicatesOutputLimitFailure(const std::string& capturedStderr) {

    // Python raises OSError with errno 27 when it fails to write to a file that has reached its size limit.
    if (capturedStderr.find("File too large") != std::string::npos ||
        capturedStderr.find("Errno 27") != std::string::npos) {

        return true;
    }

    return false;
}

} // namespace

ExecutionStatus OutcomeClassifier::classify(const RawOutcome& outcome) {

    // The child failed before successfully starting the program.
    if (outcome.startupFailed) {
        return ExecutionStatus::SystemError;
    }

    // The watchdog explicitly recorded a wall-clock timeout.
    if (outcome.killedForTime) {
        return ExecutionStatus::TimeLimitExceeded;
    }

    // Prefer the actual termination signal over stderr heuristics.
    if (outcome.signaled) {
        if (outcome.terminatingSignal == SIGXFSZ) {
            return ExecutionStatus::OutputLimitExceeded;
        }

        if (outcome.terminatingSignal == SIGXCPU) {
            return ExecutionStatus::TimeLimitExceeded;
        }

        if (outcome.terminatingSignal == SIGABRT && indicatesAllocationFailure(outcome.capturedStderr)) {
            return ExecutionStatus::MemoryLimitExceeded;
        }

        // A SIGKILL alone does not prove a timeout.
        if (outcome.terminatingSignal == SIGKILL && outcome.timeLimitMs > 0 && outcome.timeMs > outcome.timeLimitMs) {
            return ExecutionStatus::TimeLimitExceeded;
        }

        return ExecutionStatus::RuntimeError;
    }

    // Check the output limit after checking explicit termination causes.
    if (outcome.outputLimitExceeded || indicatesOutputLimitFailure(outcome.capturedStderr)) {
        return ExecutionStatus::OutputLimitExceeded;
    }

    // Interpret normal process exit.
    if (outcome.exited) {
        if (outcome.exitCode != 0 && indicatesAllocationFailure(outcome.capturedStderr)) {
            return ExecutionStatus::MemoryLimitExceeded;
        }

        if (outcome.exitCode != 0) {
            return ExecutionStatus::RuntimeError;
        }

        if (outcome.timeLimitMs > 0 && outcome.timeMs > outcome.timeLimitMs) {
            return ExecutionStatus::TimeLimitExceeded;
        }

        return ExecutionStatus::Completed;
    }

    return ExecutionStatus::SystemError;
}

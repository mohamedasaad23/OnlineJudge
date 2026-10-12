#include "core/Judge.hpp"

#include "core/Compiler.hpp"
#include "core/Evaluator.hpp"
#include "core/ProcessExecutor.hpp"

#include <cstdio>
#include <fstream>

namespace {

Verdict verdictFor(ExecutionStatus status) {
    switch (status) {
        case ExecutionStatus::TimeLimitExceeded:
            return Verdict::TimeLimitExceeded;

        case ExecutionStatus::MemoryLimitExceeded:
            return Verdict::MemoryLimitExceeded;

        case ExecutionStatus::RuntimeError:
            return Verdict::RuntimeError;

        case ExecutionStatus::OutputLimitExceeded:
            return Verdict::OutputLimitExceeded;

        case ExecutionStatus::SystemError:
            return Verdict::SystemError;

        case ExecutionStatus::Completed:
            break;
    }

    return Verdict::SystemError;
}

struct TestCleaner {
    std::string inputPath;
    std::string outputPath;

    ~TestCleaner() {
        std::remove(inputPath.c_str());
        std::remove(outputPath.c_str());
    }
};

} // namespace


JudgeResult Judge::grade(const JudgeRequest& request) {
    if (request.testCases.empty()) {
        return {Verdict::NoTestCases, 0, 0};
    }

    const PrepareResult prepared = Compiler::prepare(request.language, request.sourceCode, request.workDir);

    switch (prepared.status) {
        case PrepareStatus::InternalError:
            return {Verdict::SystemError, 0, 0};

        case PrepareStatus::CompilationError:
            return {Verdict::CompilationError, 0, 0};

        case PrepareStatus::Ready:
            break;
    }

    int maxTimeMs = 0;

    for (size_t index = 0; index < request.testCases.size(); ++index) {

        const std::string inputPath = request.workDir + "/input_" + std::to_string(index) + ".txt";
        const std::string outputPath = request.workDir + "/output_" + std::to_string(index) + ".txt";

        TestCleaner cleaner{
            inputPath,
            outputPath
        };

        std::ofstream inputFile(inputPath);

        if (!inputFile.is_open()) {
            return {Verdict::SystemError, static_cast<int>(index + 1), maxTimeMs};
        }

        inputFile << request.testCases[index].input_data;

        if (inputFile.fail()) {
            return {Verdict::SystemError, static_cast<int>(index + 1), maxTimeMs};
        }

        inputFile.close();

        if (inputFile.fail()) {
            return {Verdict::SystemError, static_cast<int>(index + 1), maxTimeMs};
        }

        const ExecutionResult rslt = ProcessExecutor::execute({request.workDir, prepared.executablePath, inputPath, outputPath, request.timeLimitMs, request.memoryLimitMb});

        if (rslt.timeMs > maxTimeMs) maxTimeMs = rslt.timeMs;

        if (rslt.status != ExecutionStatus::Completed) {
            return {verdictFor(rslt.status), static_cast<int>(index + 1), maxTimeMs};
        }

        if (!Evaluator::compareOutputs(outputPath, request.testCases[index].expected_output)) {
            return {Verdict::WrongAnswer, static_cast<int>(index + 1), maxTimeMs};
        }
    }

    return {Verdict::Accepted, 0, maxTimeMs};
}

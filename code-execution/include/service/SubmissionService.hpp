#pragma once

#include "core/Language.hpp"
#include "core/ThreadPool.hpp"

#include <cstddef>
#include <string>

class DatabaseManager;

struct GradingSpec {
    int problemId;
    int timeLimitMs;
    int memoryLimitMb;
};

enum class SubmitStatus {
    Queued,
    ServerBusy
};

struct SubmitOutcome {
    SubmitStatus status;
    int submissionId;
};


class SubmissionService {
public:
    SubmissionService(DatabaseManager& db, std::string workDir, size_t workerCount, size_t maxQueueSize);
    SubmitOutcome submit(const GradingSpec& gradingSpec, Language language, const std::string& sourceCode);

private:
    DatabaseManager& db;
    std::string workDir;
    ThreadPool pool;

    void gradeInBackground(int submissionId, GradingSpec gradingSpec, Language language, const std::string& sourceCode);
};

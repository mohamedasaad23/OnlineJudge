#include "service/SubmissionService.hpp"
#include "db/DatabaseManager.hpp"
#include "core/Judge.hpp"
#include "core/Verdict.hpp"

#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>
#include <vector>

namespace {

struct JobCleaner {
    std::string path;

    ~JobCleaner() noexcept {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

} // namespace

SubmissionService::SubmissionService(DatabaseManager& dbIn, std::string workDirIn, size_t workerCount, size_t maxQueueSize)
    : db(dbIn), workDir(std::filesystem::absolute(workDirIn).string()), pool(workerCount, maxQueueSize) {}

SubmitOutcome SubmissionService::submit(const GradingSpec& gradingSpec, Language language, const std::string& sourceCode) {

    const int submissionId = db.addSubmission(gradingSpec.problemId, canonicalName(language), sourceCode);

    try {
        pool.enqueue([this, submissionId, gradingSpec, language, sourceCode]() {
                gradeInBackground(submissionId, gradingSpec, language, sourceCode);
        });
    }
    catch (const ThreadPoolFullException&) {
        db.updateSubmissionVerdict(submissionId, toString(Verdict::Rejected), 0);
        return {
            SubmitStatus::ServerBusy,
            submissionId
        };
    }
    catch (const std::exception&) {
        db.updateSubmissionVerdict(submissionId, toString(Verdict::SystemError), 0);
        throw;
    }

    return {
        SubmitStatus::Queued,
        submissionId
    };
}

void SubmissionService::gradeInBackground(int submissionId, GradingSpec gradingSpec, Language language, const std::string& sourceCode) {
    try {
        const std::string jobDir = workDir + "/" + std::to_string(submissionId);

        JobCleaner cleaner{
            jobDir
        };

        std::filesystem::create_directories(
            jobDir
        );

        std::vector<TestCase> testCases = db.getTestCases(gradingSpec.problemId);

        const JudgeResult rslt = Judge::grade({language, sourceCode, std::move(testCases), gradingSpec.timeLimitMs, gradingSpec.memoryLimitMb, jobDir});

        db.updateSubmissionVerdict(submissionId, toString(rslt), rslt.maxTimeMs);

    }
    catch (const std::exception& error) {
        std::cerr << "Submission " << submissionId << " failed: " << error.what() << std::endl;

        try {
            db.updateSubmissionVerdict(submissionId, toString(Verdict::SystemError), 0);
        }
        catch (const std::exception& updateError) {
            std::cerr << "Failed to record submission " << submissionId << " failure: " << updateError.what() << std::endl;
        }
    }
    catch (...) {
        std::cerr << "Submission " << submissionId << " failed with an unknown error" << std::endl;

        try {
            db.updateSubmissionVerdict(submissionId, toString(Verdict::SystemError), 0);
        }
        catch (...) {
            std::cerr << "Failed to record submission " << submissionId << " failure" << std::endl;
        }
    }
}

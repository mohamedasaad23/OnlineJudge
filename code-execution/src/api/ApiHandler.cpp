#include "api/ApiHandler.hpp"

#include "db/DatabaseManager.hpp"
#include "service/SubmissionService.hpp"
#include "core/Language.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
    constexpr int kMaxTimeLimitMs = 15000; // maximum time limit in milliseconds (15 seconds)
    constexpr int kMaxMemoryLimitMb = 1024; // maximum memory limit in megabytes (1 GB)
    constexpr size_t kMaxSourceBytes = 64 * 1024; // maximum source code size in bytes (64 KB)

    bool isObject(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::Object;
    }

    bool isList(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::List;
    }

    bool isString(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::String;
    }

    bool isInteger(const crow::json::rvalue& value) {
        return value.t() == crow::json::type::Number &&
               value.nt() != crow::json::num_type::Floating_point;
    }

    crow::json::wvalue problemJson(const ProblemSummary& problem) {
        crow::json::wvalue result;
        result["problem_id"] = problem.problem_id;
        result["title"] = problem.title;
        result["time_limit"] = problem.time_limit;
        result["memory_limit"] = problem.memory_limit;
        return result;
    }

    crow::json::wvalue problemJson(const ProblemInfo& problem) {
        crow::json::wvalue result = problemJson(static_cast<const ProblemSummary&>(problem));
        result["statement"] = problem.statement;
        return result;
    }

    crow::json::wvalue submissionJson(const SubmissionSummary& submission) {
        crow::json::wvalue result;
        result["submission_id"] = submission.submission_id;
        result["problem_id"] = submission.problem_id;
        result["language"] = submission.language;
        result["verdict"] = submission.verdict;
        result["execution_time_ms"] = submission.execution_time;
        return result;
    }

    crow::json::wvalue submissionJson(const SubmissionInfo& submission) {
        crow::json::wvalue result = submissionJson(static_cast<const SubmissionSummary&>(submission));
        result["source_code"] = submission.source_code;
        return result;
    }

} // namespace

ApiHandler::ApiHandler(DatabaseManager& db, SubmissionService& submissionsIn)
    : dbManager(db), submissions(submissionsIn) {}

void ApiHandler::setupRoutes(crow::SimpleApp& app) {
    registerProblemRoutes(app);
    registerTestCaseRoutes(app);
    registerSubmissionRoutes(app);
}

void ApiHandler::registerProblemRoutes(crow::SimpleApp& app) {
    CROW_ROUTE(app, "/problems").methods(crow::HTTPMethod::POST)(
        [this](const crow::request& req) {
            auto requestJson = crow::json::load(req.body);

            if (!requestJson) return crow::response(400, "Invalid JSON"); // Bad Request 400
            if (!isObject(requestJson)) return crow::response(400, "Request body must be a JSON object");

            if (!requestJson.has("title") || !requestJson.has("statement") || !requestJson.has("time_limit") || !requestJson.has("memory_limit")) {
                return crow::response(400, "Missing required fields: title, statement, time_limit, memory_limit");
            }

            if (!isString(requestJson["title"]) || !isString(requestJson["statement"]) ||
                !isInteger(requestJson["time_limit"]) || !isInteger(requestJson["memory_limit"])) {
                return crow::response(400, "title and statement must be strings; time_limit and memory_limit must be integers");
            }

            try {
                const std::string title = requestJson["title"].s();
                const std::string statement = requestJson["statement"].s();
                const int timeLimit = requestJson["time_limit"].i();
                const int memoryLimit = requestJson["memory_limit"].i();

                if (title.empty() || statement.empty() || timeLimit <= 0 || memoryLimit <= 0 ||
                    timeLimit > kMaxTimeLimitMs || memoryLimit > kMaxMemoryLimitMb) {
                    return crow::response(400, "title/statement must be non-empty and limits must be positive and within bounds");
                }

                crow::json::wvalue result;
                result["status"] = "Problem created successfully";
                result["problem_id"] = dbManager.addProblem(title, statement, timeLimit, memoryLimit);

                return crow::response(200, result); // OK 200
            }
            catch (const DatabaseError& e) {
                return crow::response(500, std::string("Internal server error: ") + e.what()); // Internal Server Error 500
            }
            catch (const std::exception& error) {
                return crow::response(400, std::string("Invalid field types: ") + error.what()); // Bad Request 400
            }
        }
    );

    CROW_ROUTE(app, "/problems").methods(crow::HTTPMethod::GET)(
        [this]() {
            try {
                auto problems = dbManager.getAllProblems();
                crow::json::wvalue::list resultList;

                for (const auto& problem : problems) {
                    resultList.emplace_back(problemJson(problem));
                }

                // convert the list to a JSON array
                return crow::response(200, crow::json::wvalue(std::move(resultList))); // OK 200
            }
            catch (const DatabaseError& e) {
                return crow::response(500, std::string("Internal server error: ") + e.what()); // Internal Server Error 500
            }
        }
    );

    CROW_ROUTE(app, "/problems/<int>").methods(crow::HTTPMethod::GET)(
        [this](int problemId) {
            try {
                auto problem = dbManager.getProblem(problemId);
                if (!problem.has_value()) return crow::response(404, "Problem not found"); // Not Found 404

                return crow::response(200, problemJson(problem.value())); // OK 200
            }
            catch (const DatabaseError& e) {
                return crow::response(500, std::string("Internal server error: ") + e.what()); // Internal Server Error 500
            }
        }
    );
}

void ApiHandler::registerTestCaseRoutes(crow::SimpleApp& app) {
    CROW_ROUTE(app, "/test_cases").methods(crow::HTTPMethod::POST)(
        [this](const crow::request& req) {
            auto requestJson = crow::json::load(req.body);

            if (!requestJson) return crow::response(400, "Invalid JSON"); // Bad Request 400
            if (!isObject(requestJson)) return crow::response(400, "Request body must be a JSON object");

            if (!requestJson.has("problem_id")) return crow::response(400, "Missing problem_id"); // Bad Request 400

            if (!isInteger(requestJson["problem_id"])) {
                return crow::response(400, "problem_id must be an integer"); // Bad Request 400
            }

            const int problemId = requestJson["problem_id"].i();

            try {
                if (!dbManager.problemExists(problemId)) return crow::response(404, "problem_id does not exist"); // Not Found 404
                if (!requestJson.has("test_cases")) return crow::response(400, "Missing 'test_cases' array"); // Bad Request 400
                if (!isList(requestJson["test_cases"])) return crow::response(400, "'test_cases' must be an array");

                std::vector<TestCaseInput> batch;

                for (const auto& testCase : requestJson["test_cases"]) {
                    if (!isObject(testCase) || !testCase.has("input") || !testCase.has("output")) {
                        return crow::response(400, "Each test case must be an object with 'input' and 'output'");
                    }

                    if (!isString(testCase["input"]) || !isString(testCase["output"])) {
                        return crow::response(400, "'input' and 'output' must be strings");
                    }

                    batch.push_back({
                        testCase["input"].s(),
                        testCase["output"].s()
                    });
                }

                if (batch.empty()) {
                    return crow::response(400, "'test_cases' must not be empty");
                }

                dbManager.addTestCases(problemId, batch);

                crow::json::wvalue result;
                result["status"] = "Test cases added successfully";
                return crow::response(200, result);

            }
            catch (const DuplicateTestCaseError&) {
                return crow::response(409, "Duplicate test case"); // Conflict 409
            }
            catch (const DatabaseError& e) {
                return crow::response(500, std::string("Internal server error: ") + e.what()); // Internal Server Error 500
            }
            catch (const std::exception& e) {
                return crow::response(400, std::string("Error parsing test cases: ") + e.what()); // Bad Request 400
            }
        }
    );
}

void ApiHandler::registerSubmissionRoutes(crow::SimpleApp& app) {
    CROW_ROUTE(app, "/submit").methods(crow::HTTPMethod::POST)
        ([this](const crow::request& req) {
            auto requestJson = crow::json::load(req.body);

            if (!requestJson) return crow::response(400, "Invalid JSON"); // Bad Request 400
            if (!isObject(requestJson)) return crow::response(400, "Request body must be a JSON object");

            if (!requestJson.has("problem_id") || !requestJson.has("language") || !requestJson.has("source_code")) {
                return crow::response(400, "Missing required fields: problem_id, language, source_code");
            }

            if (!isInteger(requestJson["problem_id"]) ||
                !isString(requestJson["language"]) ||
                !isString(requestJson["source_code"])) {
                return crow::response(400, "problem_id must be an integer; language and source_code must be strings");
            }

            const int problemId = requestJson["problem_id"].i();
            const std::string languageName = requestJson["language"].s();
            const std::string sourceCode = requestJson["source_code"].s();

            const std::optional<Language> language = parseLanguage(languageName);
            if (!language.has_value()) {
                return crow::response(400, "Unsupported language '" + languageName + "' (supported: " +
                                            supportedLanguageNames() + ")"); // Bad Request 400
            }

            if (sourceCode.empty()) return crow::response(400, "source_code is empty"); // Bad Request 400
            if (sourceCode.size() > kMaxSourceBytes) {
                return crow::response(400, "source_code exceeds the " +
                                            std::to_string(kMaxSourceBytes / 1024) + "KB limit"); // Bad Request 400
            }

            try {
                auto problem = dbManager.getProblem(problemId);
                if (!problem.has_value()) return crow::response(404, "problem_id does not exist"); // Not Found 404

                const GradingSpec gradingSpec{
                    problem->problem_id, problem->time_limit, problem->memory_limit};
                const SubmitOutcome outcome = submissions.submit(gradingSpec, *language, sourceCode);

                if (outcome.status == SubmitStatus::ServerBusy) {
                    crow::json::wvalue result;

                    result["status"] = "Server busy, submission recorded but not queued yet";
                    result["submission_id"] = outcome.submissionId;

                    return crow::response(503, result); // Service Unavailable 503
                }

                crow::json::wvalue result;

                result["status"] = "Submitted successfully";
                result["submission_id"] = outcome.submissionId;

                return crow::response(200, result);
            }
            catch (const DatabaseError& e) {
                return crow::response(500, std::string("Internal server error: ") + e.what()); // Internal Server Error 500
            }
            catch (const std::exception& error) {
                return crow::response(500, std::string("Internal server error: ") + error.what()); // Internal Server Error 500
            }
        }
    );

    CROW_ROUTE(app, "/submissions").methods(crow::HTTPMethod::GET)
        ([this]() {
            try {
                const std::vector<SubmissionSummary> submissionsList = dbManager.getAllSubmissions();

                crow::json::wvalue::list resultList;
                for (const auto& submission : submissionsList) {
                    resultList.emplace_back(submissionJson(submission));
                }

                return crow::response(200, crow::json::wvalue(std::move(resultList))); // OK 200
            }
            catch (const DatabaseError& e) {
                return crow::response(500, std::string("Internal server error: ") + e.what()); // Internal Server Error 500
            }
        }
    );

    CROW_ROUTE(app, "/submissions/<int>").methods(crow::HTTPMethod::GET)(
        [this](int submissionId) {
            try {
                auto submission = dbManager.getSubmission(submissionId);

                if (!submission.has_value()) return crow::response(404, "Submission not found"); // Not Found 404

                return crow::response(200, submissionJson(submission.value())); // OK 200
            }
            catch (const DatabaseError& e) {
                return crow::response(500, std::string("Internal server error: ") + e.what()); // Internal Server Error 500
            }
        }
    );
}

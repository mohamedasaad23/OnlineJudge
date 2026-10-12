#pragma once

#include "crow.h"

class DatabaseManager;
class SubmissionService;

class ApiHandler {
private:
    DatabaseManager& dbManager;
    SubmissionService& submissions;

    void registerProblemRoutes(crow::SimpleApp& app);
    void registerTestCaseRoutes(crow::SimpleApp& app);
    void registerSubmissionRoutes(crow::SimpleApp& app);

public:
    ApiHandler(DatabaseManager& db, SubmissionService& submissionsIn);
    void setupRoutes(crow::SimpleApp& app);
};

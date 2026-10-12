#include "api/ApiHandler.hpp"
#include "db/DatabaseManager.hpp"
#include "service/SubmissionService.hpp"
#include "core/Verdict.hpp"

#include "crow.h" // Crow is a C++ micro web framework for building web applications and APIs.

#include <filesystem>
#include <iostream>
#include <cstdlib>
#include <string>

namespace {

struct Config {
    std::filesystem::path dataDir;
    std::filesystem::path workDir;
};

Config loadConfig() {
    // The path of the running executable (./OnlineJudge)
    const std::filesystem::path executable = std::filesystem::canonical("/proc/self/exe");
    
    // The directory containing the executable (bin/)
    const std::filesystem::path binDir = executable.parent_path();

    std::filesystem::path workDir = binDir / "work_env";
    std::filesystem::path dataDir = binDir.parent_path() / "db";

    if (const char* d = std::getenv("OJ_DATA_DIR")) dataDir = d;

    return {
        dataDir,
        workDir
    };
}

} // namespace

int main() {
    try {
        // Load configuration and create necessary directories
        const Config config = loadConfig();

        std::filesystem::create_directories(config.dataDir);
        std::filesystem::create_directories(config.workDir);

        // Initialize the database
        const std::string dbPath = (config.dataDir / "database.db").string();  
        
        DatabaseManager db(dbPath);
        db.initSchema();

        // Recover any pending submissions from a previous run and mark them as SystemError
        // This is important to ensure that submissions left in a "Pending" state due to a crash or shutdown are not left unresolved.
        const int recovered =
            db.failPendingSubmissions(
                toString(Verdict::SystemError)
            );

        if (recovered > 0) {
            std::cout
                << "Recovered "
                << recovered
                << " pending submission(s) from previous run."
                << std::endl;
        }

        // Initialize the submission service with a thread pool
        SubmissionService service(
            db,
            config.workDir.string(),
            8, // Number of worker threads
            256 // Maximum queue size
        );

        // Initialize the web server and API handler
        crow::SimpleApp app;

        ApiHandler apiHandler(
            db,
            service
        );

        apiHandler.setupRoutes(app); // Set up the API routes for problems, test cases, and submissions

        std::cout << "Online Judge Backend running on port 18080..." << std::endl;
        std::cout << "Persistent data directory: " << dbPath << std::endl;
        std::cout << "Temporary work directory: " << config.workDir.string() << std::endl;

        // Start the web server on port 18080 with multithreading enabled
        app.port(18080)
            .multithreaded()
            .run();

    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << std::endl;
        return EXIT_FAILURE; // Return a failure exit code to indicate failure
    }

    return EXIT_SUCCESS;
}

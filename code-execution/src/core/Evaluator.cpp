#include "core/Evaluator.hpp"

#include <fstream>
#include <sstream>
#include <string>

bool Evaluator::compareOutputs(const std::string& actualOutputPath, const std::string& expectedOutputText) {

    std::ifstream actualFile(actualOutputPath);
    std::istringstream expectedStream(expectedOutputText);

    if (!actualFile.is_open()) return false;

    std::string actualToken, expectedToken;

    while (true) {
        const bool hasActualToken = static_cast<bool>(actualFile >> actualToken);
        const bool hasExpectedToken = static_cast<bool>(expectedStream >> expectedToken);

        if (hasActualToken != hasExpectedToken) return false;
        if (!hasActualToken) return true;
        if (actualToken != expectedToken) return false;
    }
}

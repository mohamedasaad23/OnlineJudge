#pragma once

#include <string>

class Evaluator {
public:
    static bool compareOutputs(const std::string& actualOutputPath, const std::string& expectedOutputText);
};

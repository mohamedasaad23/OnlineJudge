#include "core/Compiler.hpp"

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>

namespace {

std::string shellQuote(const std::string& value) {
    std::string quoted = "'";

    for (char character : value) {
        if (character == '\'') quoted += "'\\''";
        else quoted += character;
    }

    quoted += "'";
    return quoted;
}

std::string secondsFromMs(int milliseconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << (milliseconds / 1000.0);
    return out.str();
}

PrepareStatus runBoundedTool( const std::string& tool, const CompileLimits& limits) {

    const std::string command = "ulimit -v " + std::to_string(limits.memoryLimitMb * 1024) + "; timeout " + secondsFromMs(limits.timeoutMs) + "s " + tool + " 2> /dev/null";

    const int rawStatus = std::system(command.c_str());

    if (rawStatus == -1 || !WIFEXITED(rawStatus)) {
        return PrepareStatus::InternalError;
    }

    const int exitCode = WEXITSTATUS(rawStatus);

    if (exitCode == 0) {
        return PrepareStatus::Ready;
    }

    if (exitCode == 126 || exitCode == 127) {
        return PrepareStatus::InternalError;
    }

    return PrepareStatus::CompilationError;
}

bool writeSource(const std::string& path, const std::string& content) {
    std::ofstream file(path);

    if (!file.is_open()) return false;

    file << content;
    file.close();

    return !file.fail();
}

} // namespace


PrepareResult Compiler::prepare(Language language, const std::string& sourceCode, const std::string& workDir, const CompileLimits& limits) {

    PrepareResult rslt;

    if (language == Language::Python) {
        const std::string sourcePath = workDir + "/solution.py";

        if (!writeSource(sourcePath, "#!/usr/bin/python3\n" + sourceCode)) return rslt;
        if (chmod(sourcePath.c_str(), 0700) != 0) return rslt;

        rslt.status = runBoundedTool("python3 -m py_compile " + shellQuote(sourcePath), limits);
        rslt.executablePath = sourcePath;
        
        return rslt;
    }

    const std::string sourcePath = workDir + "/solution.cpp";
    const std::string binaryPath = workDir + "/solution.out";

    if (!writeSource(sourcePath, sourceCode)) {
        return rslt;
    }

    rslt.status = runBoundedTool("g++ -O2 -std=c++17 " + shellQuote(sourcePath) + " -o " + shellQuote(binaryPath), limits);
    rslt.executablePath = binaryPath;

    return rslt;
}

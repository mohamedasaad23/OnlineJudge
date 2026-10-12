#include "core/ProcessExecutor.hpp"
#include "core/OutcomeClassifier.hpp"

#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr rlim_t kMaxOutputBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::size_t kMaxCapturedStderrBytes = 256 * 1024;

/*
 * One byte written to the startup pipe means the child failed before exec().
 * The pipe uses O_CLOEXEC, so a successful exec() closes the write end automatically.
 *
 * Therefore:
 *   byte available -> startup/setup failure
 *   EOF            -> exec() succeeded
 */
constexpr char kStartupFailureCode = 'F';

static void closeInheritedFds(int keepFd) {
    const unsigned first = 3, keep = static_cast<unsigned>(keepFd);
    if (keep > first) syscall(SYS_close_range, first, keep - 1, 0);
    syscall(SYS_close_range, keep + 1, ~0U, 0);
}

std::string readLimitedFile(const std::string& path, std::size_t maxBytes) {

    std::ifstream file(path, std::ios::binary);

    if (!file.is_open()) {
        return {};
    }

    std::string content;
    content.reserve(maxBytes);

    char buffer[4096];

    while (content.size() < maxBytes && file) {
        const std::size_t remaining = maxBytes - content.size();

        const std::size_t chunkSize = remaining < sizeof(buffer) ? remaining : sizeof(buffer);

        file.read(buffer, static_cast<std::streamsize>(chunkSize));

        const std::streamsize bytesRead = file.gcount();

        if (bytesRead <= 0) {
            break;
        }

        content.append(buffer, static_cast<std::size_t>(bytesRead));
    }

    return content;
}

bool setResourceLimits(const ExecutionRequest& request) {

    // CPU limit
    struct rlimit cpuLimit {};

    const int cpuSeconds = (request.timeLimitMs + 999) / 1000;

    cpuLimit.rlim_cur = static_cast<rlim_t>(cpuSeconds);
    cpuLimit.rlim_max = static_cast<rlim_t>(cpuSeconds + 1);

    if (setrlimit(RLIMIT_CPU, &cpuLimit) != 0) {
        return false;
    }

    // Memory limit
    struct rlimit memoryLimit {};

    const rlim_t memoryBytes = static_cast<rlim_t>(request.memoryLimitMb) * 1024ULL * 1024ULL;

    memoryLimit.rlim_cur = memoryBytes;
    memoryLimit.rlim_max = memoryBytes;

    if (setrlimit(RLIMIT_AS, &memoryLimit) != 0) {
        return false;
    }

    // Output file size limit
    struct rlimit outputLimit {};

    outputLimit.rlim_cur = kMaxOutputBytes;
    outputLimit.rlim_max = kMaxOutputBytes;

    if (setrlimit(RLIMIT_FSIZE, &outputLimit) != 0) {
        return false;
    }

    // Never generate core dumps for failed submissions.
    struct rlimit coreLimit {};

    coreLimit.rlim_cur = 0;
    coreLimit.rlim_max = 0;

    if (setrlimit(RLIMIT_CORE, &coreLimit) != 0) {
        return false;
    }

    return true;
}

bool fileExceedsLimit(const std::string& path) {

    struct stat info {};

    if (stat(path.c_str(), &info) != 0) {
        return false;
    }

    return static_cast<rlim_t>(info.st_size) > kMaxOutputBytes;
}

void reportStartupFailure(int startupFd) {

    const char code = kStartupFailureCode;

    // Write the failure code to the startup pipe, ignoring errors.
    const ssize_t rslt = write(startupFd, &code, sizeof(code));
    (void)rslt;

    _exit(125);
}

} // namespace

ExecutionResult ProcessExecutor::execute(const ExecutionRequest& request) {

    const std::string stderrPath = request.workDir + "/execution_stderr.txt";

    int startupPipe[2];
    // startupPipe[0] for reading
    // startupPipe[1] for writing
    // O_CLOEXEC for automatic closure on exec()

    if (pipe2(startupPipe, O_CLOEXEC) != 0) {
        return { ExecutionStatus::SystemError, 0, 0 };
    }


    /*
     * the parent process will monitor the child process and enforce time/memory limits.
     * The child process will just execute the submitted code. 
     */
    const pid_t pid = fork();
    
    // pid < 0: fork failed
    // pid > 0: parent process
    // pid == 0: child process
    if (pid < 0) {
        close(startupPipe[0]);
        close(startupPipe[1]);

        return { ExecutionStatus::SystemError, 0, 0 };
    }

    if (pid == 0) { // Child process
        close(startupPipe[0]);

        // Put the submission into its own process group.
        // All normally-created descendants inherit this group.
        if (setpgid(0, 0) != 0) {
            reportStartupFailure(startupPipe[1]);
        }

        // Programs should execute from their private submission directory rather than the server's working directory.
        if (chdir(request.workDir.c_str()) != 0) {
            reportStartupFailure(startupPipe[1]);
        }

        const int inputFd = open(request.inputPath.c_str(), O_RDONLY);

        if (inputFd < 0) {
            reportStartupFailure(startupPipe[1]);
        }

        // Redirect standard input to the input file.
        if (dup2(inputFd, STDIN_FILENO) < 0) {
            reportStartupFailure(startupPipe[1]);
        }

        if (inputFd != STDIN_FILENO) {
            close(inputFd);
        }

        const int outputFd = open(request.outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (outputFd < 0) {
            reportStartupFailure(startupPipe[1]);
        }

        // Redirect standard output to the output file.
        if (dup2(outputFd, STDOUT_FILENO) < 0) {
            reportStartupFailure(startupPipe[1]);
        }

        if (outputFd != STDOUT_FILENO) {
            close(outputFd);
        }

        const int stderrFd = open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (stderrFd < 0) {
            reportStartupFailure(startupPipe[1]);
        }

        // Redirect standard error to the error file.
        if (dup2(stderrFd, STDERR_FILENO) < 0) {
            reportStartupFailure(startupPipe[1]);
        }

        if (stderrFd != STDERR_FILENO) {
            close(stderrFd);
        }

        if (!setResourceLimits(request)) {
            reportStartupFailure(startupPipe[1]);
        }

        // Close every file descriptor inherited from the server (listening
        // socket, client connections, ...) except the startup pipe, so the
        // submitted program cannot interfere with the server's own I/O.
        closeInheritedFds(startupPipe[1]);

        /*
        * empty_env ensures that the executed process does not inherit any environment variables from the parent process.
        *
        * This is important for security and isolation, as it prevents the executed process from accessing sensitive
        * information or configurations that may be present in the parent process's environment.
        * 
        * By providing an empty environment, we can control the execution context of the child
        */
        char* empty_env[] = { nullptr };

        execle(
            request.executablePath.c_str(),
            request.executablePath.c_str(),
            static_cast<char*>(nullptr),
            empty_env
        );

        reportStartupFailure(startupPipe[1]);
    }

    close(startupPipe[1]);

    /*
     * The child creates its own process group immediately after fork().
     *
     * The parent also attempts to establish the group here to minimize
     * the race window. EACCES/ESRCH are harmless because the child may
     * already have exec'd or exited.
     */
    if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {

        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);

        close(startupPipe[0]);

        return { ExecutionStatus::SystemError, 0, 0 };
    }

    /*
     * Guarded by mtx (not std::atomic) so that every write to it happens
     * while holding the lock. That is what makes the notify_one() below
     * race-free: cv.wait_for() re-checks the predicate under the same
     * lock before it actually goes to sleep, so a notification can never
     * be sent (and lost) between the predicate check and the wait.
     */
    bool process_finished = false;
    
    std::mutex mtx;
    std::condition_variable cv;

    bool killedForTime = false;

    /*
     * Watchdog handles wall-clock timeout.
     *
     * CPU time is already enforced independently via RLIMIT_CPU / the
     * classifier's timeMs check, so the watchdog's real job is to catch
     * processes that are merely sleeping or stuck waiting (e.g. on I/O)
     * without burning CPU. Give it real slack above the declared time
     * limit so that host scheduling noise under concurrent submissions
     * doesn't turn a correct, fast solution into a spurious TLE.
     */
    const int watchdogTimeoutMs = 2 * request.timeLimitMs + 500; // 500ms of extra slack for scheduling noise 

    // Kill the entire process group rather than just the original submission process.
    std::thread watchdog([&]() {
        std::unique_lock<std::mutex> lock(mtx);

        if (!cv.wait_for(lock, std::chrono::milliseconds(watchdogTimeoutMs), [&] { return process_finished; })) {

            killedForTime = true;
            kill(-pid, SIGKILL); // Kill the entire process group.
        }
    });

    int waitStatus = 0;
    struct rusage usage {};

    while (true) {
        // Blocking wait: no busy polling.
        const pid_t result = wait4(pid, &waitStatus, 0, &usage);

        if (result == pid) {
            break;
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }

        if (result < 0) {
            close(startupPipe[0]);
            std::remove(stderrPath.c_str());

            {
                std::lock_guard<std::mutex> lock(mtx);
                process_finished = true;
            }
            cv.notify_one();

            watchdog.join();

            return { ExecutionStatus::SystemError, 0, 0 };
        }
    }

    //  The main process has finished.
    {
        std::lock_guard<std::mutex> lock(mtx);
        process_finished = true;
    }
    cv.notify_one();

    watchdog.join();

    /*
     * The original process may have exited while leaving forked descendants behind.
     *
     * Clean up anything still inside the submission process group.
     *
     * ESRCH is expected when the group no longer exists.
     */
    if (kill(-pid, SIGKILL) != 0 && errno != ESRCH) {

        close(startupPipe[0]);

        std::remove(stderrPath.c_str());

        return { ExecutionStatus::SystemError, 0, 0 };
    }


    // Read the startup pipe to determine if the child failed before exec().
    char startupFailureCode = 0;

    const ssize_t startupBytes = read(startupPipe[0], &startupFailureCode, sizeof(startupFailureCode));

    close(startupPipe[0]);

    if (startupBytes < 0) {
        std::remove(stderrPath.c_str());

        return { ExecutionStatus::SystemError, 0, 0 };
    }

    const bool outputLimitExceeded = fileExceedsLimit(request.outputPath);

    RawOutcome outcome;

    outcome.startupFailed = startupBytes == 1 && startupFailureCode == kStartupFailureCode;
    outcome.killedForTime = killedForTime;
    outcome.outputLimitExceeded = outputLimitExceeded;
    outcome.exited = WIFEXITED(waitStatus);
    outcome.signaled = WIFSIGNALED(waitStatus);
    outcome.exitCode = outcome.exited ? WEXITSTATUS(waitStatus) : -1;
    outcome.terminatingSignal = outcome.signaled ? WTERMSIG(waitStatus) : 0;

    // CPU time = user + system.
    outcome.timeMs = static_cast<int>(
        usage.ru_utime.tv_sec * 1000 +
        usage.ru_utime.tv_usec / 1000 +
        usage.ru_stime.tv_sec * 1000 +
        usage.ru_stime.tv_usec / 1000
    );

    outcome.memoryKb = static_cast<int>(usage.ru_maxrss);
    outcome.timeLimitMs = request.timeLimitMs;
    outcome.memoryLimitMb = request.memoryLimitMb;
    outcome.capturedStderr = readLimitedFile(stderrPath, kMaxCapturedStderrBytes);

    // Remove the temporary stderr file.
    std::remove(stderrPath.c_str());

    const ExecutionStatus status = OutcomeClassifier::classify(outcome);

    return { status, outcome.timeMs, outcome.memoryKb };
}

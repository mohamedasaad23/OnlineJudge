# Online Judge Backend

A multithreaded online judge written in modern **C++17**. It accepts **C++** and **Python 3** submissions over a REST API, compiles and runs them against stored test cases inside **resource-limited child processes**, and records verdicts in SQLite database.

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![CMake](https://img.shields.io/badge/build-CMake%203.18%2B-064F8C.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)
![Tests](https://img.shields.io/badge/e2e%20tests-85%20passing-brightgreen.svg)

> **Scope note.** This is a learning project, not a production judge. It deliberately skips heavy sandboxing (namespaces, seccomp, cgroups) to keep the design focused on concurrency, ownership, and resource control.

---

## 🎯 Highlights

| | |
|---|---|
| 🧵 **Multithreading** | Hand-written bounded thread pool, per-run watchdog threads, mutex-protected shared state, graceful shutdown |
| 🧠 **Ownership & RAII** | No raw `new` / `delete`. `shared_ptr`, `unique_ptr` with custom deleters, and scope guards own every resource |
| 🔒 **Hard resource limits** | CPU time, wall-clock time, memory, output size, core dumps, environment, inherited file descriptors |
| 🚦 **Backpressure** | Bounded queue answers **HTTP 503** instead of growing memory without limit |
| 💥 **Crash recovery** | Submissions left `Pending` by a previous run are marked `System Error` on startup |
| ✅ **Tested end to end** | 85 automated checks, including a concurrent stress test and a queue-overflow test |

---

## ✨ Features

### Judging
- C++17 (`g++ -O2`) and Python 3 (syntax-checked with `py_compile`)
- Language aliases: `cpp`, `c++`, `cpp17`, `python`, `python3`, `py`
- Multiple test cases per problem; grading stops at the first failing test and reports its number
- Token-based, whitespace-insensitive output comparison
- Precise verdict classification from exit status, termination signal, and stderr

### Platform
- REST API built with [Crow](https://github.com/CrowCpp/Crow)
- Asynchronous grading: `POST /submit` returns immediately, clients poll for the verdict
- SQLite persistence (WAL mode, prepared statements, RAII wrappers)
- Transactional test-case upload: a batch containing a duplicate is rolled back entirely
- Portable layout: all paths are resolved relative to the executable

---

## 🏗 Architecture

```text
                               HTTP (Crow, multithreaded)
                                         │
                                         ▼
                                ┌─────────────────┐
                                │   ApiHandler    │   validation, JSON, status codes
                                └────────┬────────┘
                                         |
                                         ▼
                               ┌────────────────────┐
                               │ SubmissionService  │   save as Pending, enqueue job
                               └─────────┬──────────┘
                                         |
                                         ▼
                               ┌────────────────────┐
                               │     ThreadPool     │   bounded queue (256), 8 workers
                               └─────────┬──────────┘
                                         |  (one worker per submission)
                                         ▼   
                                    ┌─────────┐
                                    │  Judge  │   compile once; per test: run → compare
                                    └────┬────┘
                                         |
                 ┌───────────────────────┼───────────────────────┐
                 ▼                       ▼                       ▼
         ┌──────────────────┐   ┌─────────────────┐      ┌────────────┐
         │     Compiler     │   │ ProcessExecutor │      │ Evaluator  │
         │                  │   │                 │      │            │
         │ g++ / py_compile │   │   fork + exec   │      │   token    │
         └──────────────────┘   │     rlimits     │      │ comparison │
                                │ watchdog thread │      └────────────┘
                                └────────┬────────┘
                                         ▼
                                 OutcomeClassifier
                       (exit status / signal / stderr → verdict)
     DatabaseManager (SQLite, mutex-guarded) is used by ApiHandler and SubmissionService.
```

| Layer | Responsibility |
|---|---|
| `ApiHandler` | Parses and validates requests, maps errors to HTTP status codes |
| `SubmissionService` | Persists the submission, hands the job to the pool, handles queue-full rejection |
| `ThreadPool` | Bounded task queue and worker threads |
| `Judge` | Orchestrates compile → per-test run → compare, and decides the final verdict |
| `Compiler` | Builds C++ / validates Python under time and memory limits |
| `ProcessExecutor` | Runs one test in a limited child process with a watchdog |
| `OutcomeClassifier` | Pure function: raw process outcome → verdict category |
| `Evaluator` | Compares program output with the expected output |
| `DatabaseManager` | All SQL, serialized behind a mutex |

---

## 🔄 Request lifecycle

```text
Client               ApiHandler       SubmissionService     Worker thread               Judge
  ┼                      ┼                    ┼                   ┼                       ┼
  │  POST /submit        │                    │                   │                       │
  │ ──────────────────▶ │ validate           │                   │                       │
  │                      │──────────────────▶│ INSERT (Pending)  │                       │
  │                      │                    │   enqueue(job)    │                       │
  │  200 {id}            │                    │      ────▶       │                       │
  │ ◀────────────────── │                    │                   │ create work dir       │
  │                      │                    │                   │ load test cases       │ Compile
  │                      │                    │                   │─────────────────────▶│
  │  GET /submissions/id |                    │                   │                       │ for each test:
  │ ──────────────────▶ │  verdict: Pending  │                   │                       │    fork/exec + limits
  │ ◀────────────────── │                    │                   │                       │    compare output
  │                      │                    │                   │◀─────────────────────│
  │                      │                    │                   │ UPDATE verdict        │ Verdict
  │                      │                    │                   │ remove work dir       │
  │  GET /submissions/id |                    │                   │                       │
  │ ──────────────────▶ │  verdict: Accepted │                   │                       │
  │ ◀────────────────── │                    │                   │                       │
```

1. `POST /submit` validates the request, stores the submission as `Pending`, and queues a job. If the queue is full, the submission is recorded as *Rejected* and the client receives **503**.
2. A worker creates a private work directory and compiles the code (`g++ -O2 -std=c++17`; Python is syntax-checked with `py_compile`).
3. Each test case runs in a limited child process; its output is compared token by token with the expected output.
4. Grading **stops at the first failing test**. The verdict and the maximum CPU time are written to the database, and the work directory is removed.
5. The client polls `GET /submissions/<id>` until the verdict is no longer `Pending`.

---

## 🧵 Concurrency model

Defaults: **8 worker threads**, **256 queued jobs** (set in `src/main.cpp`). Crow additionally runs its HTTP handlers on its own thread pool (`multithreaded()`), so receiving requests and grading never block each other.

| Concern | Implementation | Where |
|---|---|---|
| Asynchronous grading | `POST /submit` stores the submission and returns immediately; a worker grades it later | `SubmissionService` |
| Worker pool | Custom `ThreadPool`: `std::thread` workers, `std::mutex` + `std::condition_variable` task queue, `std::packaged_task` / `std::future` results | `core/ThreadPool.hpp` |
| Backpressure | The queue is bounded. When it is full, `enqueue` throws `ThreadPoolFullException` and the API answers **503** | `ThreadPool`, `SubmissionService`, `ApiHandler` |
| Graceful shutdown | The destructor sets `stop`, wakes every worker, lets them drain the queue, then joins them | `ThreadPool::~ThreadPool` |
| Wall-clock watchdog | Each run spawns a watchdog thread that waits on a condition variable with a timeout and kills the **whole process group** when the deadline passes. The flag is guarded by the same mutex the wait uses, so a wake-up can never be lost | `ProcessExecutor.cpp` |
| Shared state | The database connection is the only shared mutable object, serialized with `std::mutex` + `std::lock_guard`. Everything else is per-submission: each job works in its own directory | `DatabaseManager` |
| Exception safety in workers | Every job body catches all exceptions, records `System Error`, and never lets a worker thread die | `SubmissionService::gradeInBackground` |

**Why a custom pool?** It keeps the queue bound, the rejection path, and the shutdown order explicit and testable. The test suite proves the 503 path by overflowing the queue on purpose.

---

## 🧠 Ownership and memory safety

There is no manual `new` / `delete` anywhere in the code base. Every resource has exactly one owner and is released on every exit path, including exceptions.

| Resource | Owner | Mechanism |
|---|---|---|
| Queued grading job | the queue, then a worker | `std::shared_ptr<std::packaged_task<R()>>`. `std::function` needs a copyable callable and `packaged_task` is move-only, so the shared pointer is the bridge; the caller keeps the matching `std::future` |
| SQLite prepared statement | `Statement` | `std::unique_ptr<sqlite3_stmt, StmtDeleter>`; the custom deleter calls `sqlite3_finalize`, even while an exception unwinds |
| SQLite connection | `DatabaseManager` | Closed in the destructor under the lock; copying is deleted |
| Per-submission work directory | `JobCleaner` | Scope guard: `remove_all` runs on every path |
| Per-test input / output files | `TestCleaner` | Scope guard inside the judging loop |
| Mutexes | `std::lock_guard` / `std::unique_lock` | Never locked or unlocked manually |
| Pipes and file descriptors | `ProcessExecutor` | Closed on every path; the startup pipe uses `O_CLOEXEC` so a successful `exec` closes it automatically |

---

## 🔒 Resource limits

Every test runs in a `fork` / `exec` child with the following protections:

| Limit | Mechanism | Verdict |
|---|---|---|
| **CPU time** | `RLIMIT_CPU` (whole seconds, rounded up; soft limit delivers `SIGXCPU`, hard limit is one second higher) plus a check of the measured CPU time against the exact limit | Time Limit Exceeded |
| **Wall-clock time** | Watchdog thread fires after `2 × limit + 500 ms` and sends `SIGKILL` to the **entire process group**; catches sleeping or blocked programs | Time Limit Exceeded |
| **Memory** | `RLIMIT_AS` set to the problem's `memory_limit` | Memory Limit Exceeded (detected from `std::bad_alloc` / `MemoryError`) |
| **Output size** | `RLIMIT_FSIZE` = 64 MiB plus an explicit size check on the output file | Output Limit Exceeded |
| **Core dumps** | `RLIMIT_CORE = 0` | n/a |
| **Environment** | Child starts with an **empty environment** (`execle`) | n/a |
| **Inherited descriptors** | Every descriptor except the startup pipe is closed with `close_range` before `exec`, so submissions cannot touch the server's sockets | n/a |
| **Leftover processes** | The child gets its own process group; the whole group is killed after the run | n/a |
| **Compilation** | `ulimit -v 512 MB` and `timeout 10 s` | Compilation Error |

Per-request limits: source code up to **64 KB**, `time_limit` up to **15 000 ms**, `memory_limit` up to **1 024 MB**.

### Startup pipe

The child reports setup failures through a one-byte message on an `O_CLOEXEC` pipe: if the byte arrives, the child failed before `exec` (*System Error*); if the pipe closes silently, `exec` succeeded. This is why a user program that legitimately returns exit code 125 is reported as *Runtime Error* and never confused with a judge failure.

---

## 📁 Project structure

```text
.
├── README.md
├── code-execution/
│   ├── CMakeLists.txt
│   ├── test.sh
│   ├── include/
│   │   ├── api/ApiHandler.hpp
│   │   ├── core/
│   │   │   ├── Compiler.hpp
│   │   │   ├── Evaluator.hpp
│   │   │   ├── Judge.hpp
│   │   │   ├── Language.hpp
│   │   │   ├── Models.hpp
│   │   │   ├── OutcomeClassifier.hpp
│   │   │   ├── ProcessExecutor.hpp
│   │   │   ├── ThreadPool.hpp
│   │   │   └── Verdict.hpp
│   │   ├── db/DatabaseManager.hpp
│   │   └── service/SubmissionService.hpp
│   └── src/
│       ├── api/
│       ├── core/
│       ├── db/
│       ├── service/
│       └── main.cpp
├── bin/                  # generated: binary and runtime files
│   ├── OnlineJudge
│   └── work_env/
├── build/                # generated: CMake build tree
└── db/                   # generated: SQLite database
    └── database.db
```

---

## 🚀 Getting started

### Prerequisites

- Linux with kernel **5.9+** (the child uses `close_range`)
- CMake **3.18+** and a C++17 compiler (`g++`)
- `python3` (used by the judge for Python submissions and by `test.sh`)
- SQLite3 development files
- Internet access on the first configure ([Crow](https://github.com/CrowCpp/Crow) is fetched by CMake)

```bash
sudo apt install build-essential cmake python3 libsqlite3-dev libasio-dev curl
```

### Build

```bash
# from the project root
cmake -S code-execution -B build
cmake --build build -j
```

The binary is written to `bin/OnlineJudge`.

### Run

```bash
./bin/OnlineJudge
```

The server listens on **port 18080**. Paths are resolved relative to the executable:

| What | Location |
|---|---|
| Database | `db/database.db` |
| Work directories | `bin/work_env/` |

### Quick demo

```bash
# 1. create a problem (time_limit in ms, memory_limit in MB)
curl -X POST localhost:18080/problems -H 'Content-Type: application/json' -d '{
  "title": "A+B",
  "statement": "Print a+b",
  "time_limit": 1000,
  "memory_limit": 256
}'
# {"status":"Problem created successfully","problem_id":1}

# 2. add test cases
curl -X POST localhost:18080/test_cases -H 'Content-Type: application/json' -d '{
  "problem_id": 1,
  "test_cases": [{"input": "1 2\n", "output": "3\n"}]
}'

# 3. submit a solution
curl -X POST localhost:18080/submit -H 'Content-Type: application/json' -d '{
  "problem_id": 1,
  "language": "cpp",
  "source_code": "#include <iostream>\nint main(){int a,b;std::cin>>a>>b;std::cout<<a+b;}"
}'
# {"status":"Submitted successfully","submission_id":1}

# 4. poll the verdict
curl localhost:18080/submissions/1
# {"submission_id":1,"problem_id":1,"language":"cpp","verdict":"Accepted","execution_time_ms":2,"source_code":"..."}
```

---

## 📡 API reference

| Method | Path | Description |
|---|---|---|
| `POST` | `/problems` | Create a problem |
| `GET` | `/problems` | List problems |
| `GET` | `/problems/<id>` | Problem details (including the statement) |
| `POST` | `/test_cases` | Add a batch of test cases to a problem |
| `POST` | `/submit` | Submit a solution |
| `GET` | `/submissions` | List submissions (newest first) |
| `GET` | `/submissions/<id>` | Submission details (verdict, time, source) |

### Status codes

| Code | Meaning |
|---|---|
| `200` | OK |
| `400` | Invalid input (malformed JSON, missing / wrong-typed fields, out-of-range limits, unsupported language, empty or oversized source) |
| `404` | Unknown problem or submission |
| `409` | Duplicate test case |
| `503` | Server busy: the submission queue is full |
| `500` | Internal error |

### Request bodies

**`POST /problems`**

| Field | Type | Constraint |
|---|---|---|
| `title` | string | non-empty |
| `statement` | string | non-empty |
| `time_limit` | integer (ms) | 1 – 15 000 |
| `memory_limit` | integer (MB) | 1 – 1 024 |

**`POST /test_cases`**

| Field | Type | Notes |
|---|---|---|
| `problem_id` | integer | must exist |
| `test_cases` | array of `{input, output}` | non-empty; atomic: one duplicate rolls back the whole batch |

**`POST /submit`**

| Field | Type | Notes |
|---|---|---|
| `problem_id` | integer | must exist |
| `language` | string | `cpp`, `c++`, `cpp17`, `python`, `python3`, `py` |
| `source_code` | string | non-empty, up to 64 KB |

### Limits at a glance

| What | Value |
|---|---|
| Time limit per problem | 1 – 15 000 ms |
| Memory limit per problem | 1 – 1 024 MB |
| Source code size | 64 KB |
| Program output | 64 MiB |
| Compilation | 10 s, 512 MB |
| Worker threads / queue size | 8 / 256 |

---

## 🏁 Verdicts

| Verdict | Meaning |
|---|---|
| `Accepted` | All tests passed |
| `Wrong Answer on test N` | Output differs from the expected tokens on test N |
| `Compilation Error` | `g++` failed, or the Python file has a syntax error |
| `Time Limit Exceeded on test N` | CPU limit or wall-clock watchdog triggered |
| `Memory Limit Exceeded on test N` | Allocation failed under the memory limit |
| `Runtime Error on test N` | Non-zero exit code or fatal signal |
| `Output Limit Exceeded on test N` | More than 64 MiB written |
| `System Error - please resubmit` | The judge itself failed (also used for crash recovery) |
| `There are no test cases` | The problem has no test cases yet |
| `Rejected - server busy, please resubmit` | The queue was full |

---

## 🧪 Testing

`code-execution/test.sh` is an end-to-end suite that drives a **live server** over HTTP. It needs `bash`, `curl`, and `python3`.

```bash
# terminal 1: start the server
./bin/OnlineJudge

# terminal 2: run the suite
./code-execution/test.sh
```

Expected result:

```text
-------------------------------------------
PASSED: 85   FAILED: 0   SKIPPED: 0
-------------------------------------------
```

The script exits with a non-zero status and lists the failing checks if anything breaks, so it can be used directly in CI.

### Options

| Variable | Default | Meaning |
|---|---|---|
| `BASE_URL` | `http://localhost:18080` | Server address |
| `STRESS_COUNT` | `40` | Simultaneous submissions in the stress test |
| `BACKPRESSURE_COUNT` | `400` | Submissions fired by the queue-overflow test (must exceed workers + queue = 264) |

The backpressure test makes every submission sleep for two seconds, so the whole suite takes a little longer than a minute and leaves the server busy for a short while afterwards.

### What is covered

| Area | Cases |
|---|---|
| **Input validation** | Malformed JSON, missing / wrong-typed fields, empty values, limits out of bounds, oversized source, unknown problem / submission, unsupported language |
| **Persistence** | `GET` round-trips for problems and submissions, canonical language names for aliases |
| **Transactions** | Duplicate inside a batch, duplicate of a stored case, valid + duplicate batch rolled back completely |
| **Verdicts** | Accepted, Wrong Answer (extra / missing tokens, whitespace tolerance), Compilation Error (C++ and Python) |
| **Multiple tests** | Failure reported on the correct test number |
| **Runtime errors** | Non-zero exit, exit code 125, SIGSEGV, SIGFPE, uncaught exception (SIGABRT), Python exception |
| **Limits** | CPU and wall-clock TLE (busy loop, sleeping program, exact 1000 ms edge case), memory limit (C++ and Python), output limit, exact 64 MiB boundary |
| **Isolation** | Empty environment, orphaned child processes killed with the process group |
| **Concurrency** | 40 simultaneous submissions graded correctly with unique ids; queue overflow answered with 503 and recorded as *Rejected* |

---

## 🧭 Design decisions

- **Process per run, not thread per run.** `RLIMIT_AS`, `RLIMIT_CPU`, and the ability to `SIGKILL` a runaway program only make sense for a process. Threads are used where they fit: the worker pool and the watchdog.
- **Bounded queue + 503.** An unbounded queue turns a traffic spike into an out-of-memory crash. Rejecting early keeps the server healthy and gives clients a clear retry signal.
- **Wall-clock slack.** The watchdog allows `2 × limit + 500 ms`, so scheduler noise under load cannot turn a correct, fast solution into a spurious TLE. The exact CPU limit is still checked separately.
- **Soft/hard CPU limit gap.** The hard `RLIMIT_CPU` is one second above the soft one, so a CPU hog receives `SIGXCPU` (a clear TLE signal) instead of an anonymous `SIGKILL`.
- **Signals before stderr.** The classifier prefers the real termination signal and uses stderr text only where the runtime hides the cause (`bad_alloc`, Python `MemoryError`, `EFBIG`).
- **Single SQLite connection behind a mutex.** Simple and correct. WAL mode keeps external readers (such as the `sqlite3` CLI) from blocking the writer. A connection per thread is the next step if the database becomes a bottleneck.
- **Header-only `ThreadPool`.** `enqueue` is a template over the callable, so the implementation lives in the header.

---

## ⚠️ Limitations

This project is **not** a production judge.

- **No sandbox.** Submitted programs run with the privileges of the server user: they can read files, open network connections, and nothing limits how many processes they create. Never expose this server to untrusted users; run it inside a container or vm.
- Linux only (`fork`, `wait4`, `pipe2`, `close_range`, `/proc/self/exe`).
- No authentication and no pagination.
- `RLIMIT_AS` limits *virtual* memory, which is stricter than resident memory for some runtimes.
- Output comparison is token-based: no floating-point tolerance and no special checkers.
- Python submissions run through a `#!/usr/bin/python3` shebang.

---

## 🗺 Roadmap

- Run submissions in Docker / namespaces / seccomp / cgroups (PID and network isolation)
- Per-test verdict details and special checkers for problems with several valid answers
- Authentication, pagination, and a connection pool for the database
- Metrics for queue depth and grading latency
- CI pipeline that builds the project and runs `test.sh`

---

## 🛠 Tech stack

| | |
|---|---|
| **Language** | C++17 |
| **Build** | CMake 3.18+ (`FetchContent` for dependencies) |
| **Web framework** | [Crow](https://github.com/CrowCpp/Crow) v1.3.4 |
| **Database** | SQLite 3 |
| **Concurrency** | `std::thread`, `std::mutex`, `std::condition_variable`, `std::packaged_task` |
| **OS interfaces** | `fork`, `execle`, `wait4`, `setrlimit`, `setpgid`, `pipe2`, `close_range` |
| **Testing** | Bash + curl + python3 end-to-end suite |

---

**Last updated**: October 2026

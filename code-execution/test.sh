#!/usr/bin/env bash
#
# End-to-end tests for the Online Judge REST API.
#
# Requirements: bash, curl, python3 (python3 is only used to build and parse
# JSON safely, so test programs can be written as normal multi-line source).
#
# Usage
#   terminal 1 - start the server with a throw-away database:
#       OJ_DATA_DIR=/tmp/oj-test ./bin/OnlineJudge
#   terminal 2 - run the tests:
#       ./code-execution/test.sh
#
# Environment variables
#   BASE_URL            server address                          (default http://localhost:18080)
#   STRESS_COUNT        concurrent submissions in stress test   (default 40)
#   BACKPRESSURE_COUNT  submissions fired by the backpressure test (default 400,
#                       must exceed workers + queue = 264)
#
# Note: the backpressure test takes about a minute because every submission
# sleeps for two seconds. It is part of the default run.

set -u

BASE_URL="${BASE_URL:-http://localhost:18080}"
STRESS_COUNT="${STRESS_COUNT:-40}"
BACKPRESSURE_COUNT="${BACKPRESSURE_COUNT:-400}"

for tool in curl python3; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Missing required tool: $tool" >&2
        exit 2
    fi
done

WORK_DIR="$(mktemp -d)"
BODY_FILE="$WORK_DIR/body.json"
trap 'rm -rf "$WORK_DIR"' EXIT

PASS=0
FAIL=0
SKIP=0
FAILED_TESTS=()

# ---------------------------------------------------------------------------
# Output helpers
# ---------------------------------------------------------------------------

if [[ -t 1 ]]; then
    C_RESET=$'\033[0m'
    C_BLUE=$'\033[1;34m'
    C_GREEN=$'\033[1;32m'
    C_RED=$'\033[1;31m'
    C_YELLOW=$'\033[1;33m'
else
    C_RESET=''
    C_BLUE=''
    C_GREEN=''
    C_RED=''
    C_YELLOW=''
fi

section() {
    printf '\n%s== %s ==%s\n' "$C_BLUE" "$1" "$C_RESET"
}

pass() {
    printf '  %s[PASS]%s %s\n' "$C_GREEN" "$C_RESET" "$1"
    PASS=$((PASS + 1))
}

fail() {
    printf '  %s[FAIL]%s %s\n         %s\n' "$C_RED" "$C_RESET" "$1" "$2"
    FAIL=$((FAIL + 1))
    FAILED_TESTS+=("$1")
}

skip() {
    printf '  %s[SKIP]%s %s (%s)\n' "$C_YELLOW" "$C_RESET" "$1" "$2"
    SKIP=$((SKIP + 1))
}

# ---------------------------------------------------------------------------
# JSON helpers
# ---------------------------------------------------------------------------

# json_quote STRING -> prints STRING as a JSON string literal (with quotes)
json_quote() {
    python3 -c 'import json, sys; sys.stdout.write(json.dumps(sys.argv[1]))' "$1"
}

# json_get FIELD [FILE] -> prints a top-level field of a JSON object ("" if missing)
json_get() {
    python3 - "$1" "${2:-$BODY_FILE}" <<'PY'
import json, sys

try:
    with open(sys.argv[2]) as handle:
        value = json.load(handle).get(sys.argv[1], "")
except Exception:
    value = ""

print(value, end="")
PY
}

# list_contains FIELD VALUE [FILE] -> prints yes/no: does a JSON array contain an
# object whose FIELD equals VALUE?
list_contains() {
    python3 - "$1" "$2" "${3:-$BODY_FILE}" <<'PY'
import json, sys

try:
    with open(sys.argv[3]) as handle:
        items = json.load(handle)
    found = any(str(item.get(sys.argv[1])) == sys.argv[2] for item in items)
except Exception:
    found = False

print("yes" if found else "no", end="")
PY
}

# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

# http METHOD PATH [JSON_BODY | @FILE]
# Prints the HTTP status code. The response body is stored in $BODY_FILE.
http() {
    local method="$1"
    local path="$2"
    local data="${3:-}"

    local args=(-s -o "$BODY_FILE" -w '%{http_code}' -X "$method" "$BASE_URL$path")

    if [[ -n "$data" ]]; then
        args+=(-H 'Content-Type: application/json' --data-binary "$data")
    fi

    curl "${args[@]}"
}

body() {
    cat "$BODY_FILE"
}

# ---------------------------------------------------------------------------
# Assertions
# ---------------------------------------------------------------------------

assert_eq() {
    local description="$1" expected="$2" actual="$3"

    if [[ "$actual" == "$expected" ]]; then
        pass "$description"
    else
        fail "$description" "expected '$expected', got '$actual'"
    fi
}

assert_code() {
    local description="$1" expected="$2" actual="$3"

    if [[ "$actual" == "$expected" ]]; then
        pass "$description"
    else
        fail "$description" "expected HTTP $expected, got HTTP $actual: $(body | head -c 200)"
    fi
}

assert_match() {
    local description="$1" pattern="$2" actual="$3"

    if [[ "$actual" =~ $pattern ]]; then
        pass "$description"
    else
        fail "$description" "'$actual' does not match /$pattern/"
    fi
}

assert_contains() {
    local description="$1" haystack="$2" needle="$3"

    if [[ "$haystack" == *"$needle"* ]]; then
        pass "$description"
    else
        fail "$description" "'$needle' not found in '${haystack:0:200}'"
    fi
}

# ---------------------------------------------------------------------------
# Domain helpers
# ---------------------------------------------------------------------------

# create_problem TITLE TIME_LIMIT_MS MEMORY_LIMIT_MB -> prints problem_id
create_problem() {
    local title
    title="$(json_quote "$1")"

    http POST /problems \
        "{\"title\":$title,\"statement\":\"Test problem\",\"time_limit\":$2,\"memory_limit\":$3}" \
        >/dev/null

    json_get problem_id
}

# add_test PROBLEM_ID INPUT OUTPUT   (raw strings, may contain newlines)
add_test() {
    local input output
    input="$(json_quote "$2")"
    output="$(json_quote "$3")"

    http POST /test_cases \
        "{\"problem_id\":$1,\"test_cases\":[{\"input\":$input,\"output\":$output}]}" \
        >/dev/null
}

# write_submit_body FILE PROBLEM_ID LANGUAGE SOURCE
write_submit_body() {
    local source
    source="$(json_quote "$4")"

    printf '{"problem_id":%s,"language":"%s","source_code":%s}' "$2" "$3" "$source" > "$1"
}

# submit PROBLEM_ID LANGUAGE SOURCE -> prints submission_id
submit() {
    local source
    source="$(json_quote "$3")"

    http POST /submit \
        "{\"problem_id\":$1,\"language\":\"$2\",\"source_code\":$source}" \
        >/dev/null

    json_get submission_id
}

# wait_for_verdict SUBMISSION_ID [TIMEOUT_SECONDS] -> prints the final verdict
# Uses its own private temp file, so it is safe to call from concurrent contexts.
wait_for_verdict() {
    local submission_id="$1"
    local timeout="${2:-15}"
    local max_attempts=$((timeout * 4))
    local attempts=0
    local tmp verdict

    if [[ -z "$submission_id" ]]; then
        echo "SUBMIT_FAILED"
        return 0
    fi

    tmp="$(mktemp -p "$WORK_DIR")"

    while (( attempts < max_attempts )); do
        curl -s -o "$tmp" "$BASE_URL/submissions/$submission_id"
        verdict="$(json_get verdict "$tmp")"

        if [[ -n "$verdict" && "$verdict" != "Pending" ]]; then
            rm -f "$tmp"
            echo "$verdict"
            return 0
        fi

        sleep 0.25
        attempts=$((attempts + 1))
    done

    rm -f "$tmp"
    echo "TIMEOUT"
}

# expect_verdict DESCRIPTION EXPECTED PROBLEM_ID LANGUAGE SOURCE [TIMEOUT_SECONDS]
expect_verdict() {
    local description="$1" expected="$2" problem_id="$3" language="$4" source="$5"
    local timeout="${6:-15}"
    local submission_id

    submission_id="$(submit "$problem_id" "$language" "$source")"
    assert_eq "$description" "$expected" "$(wait_for_verdict "$submission_id" "$timeout")"
}

# define VARIABLE <<'EOF' ... EOF  -> stores the heredoc verbatim in VARIABLE
define() {
    IFS= read -r -d '' "$1" || true
}

# ---------------------------------------------------------------------------
# Test programs
# ---------------------------------------------------------------------------

define CPP_SUM <<'EOF'
#include <iostream>

int main() {
    int a, b;
    std::cin >> a >> b;
    std::cout << a + b << "\n";
}
EOF

define CPP_SUM_WRONG <<'EOF'
#include <iostream>

int main() {
    int a, b;
    std::cin >> a >> b;
    std::cout << a + b + 1 << "\n";
}
EOF

define CPP_SUM_WRONG_ON_SECOND_TEST <<'EOF'
#include <iostream>

int main() {
    int a, b;
    std::cin >> a >> b;
    std::cout << (a == 10 ? a + b + 1 : a + b) << "\n";
}
EOF

define CPP_SUM_CRASH_ON_THIRD_TEST <<'EOF'
#include <iostream>

int main() {
    int a, b;
    std::cin >> a >> b;
    if (a < 0) return 3;
    std::cout << a + b << "\n";
}
EOF

define CPP_SUM_ODD_WHITESPACE <<'EOF'
#include <iostream>

int main() {
    int a, b;
    std::cin >> a >> b;
    std::cout << "  \n\n   " << a + b << "   \n\n\n";
}
EOF

define CPP_SUM_EXTRA_TOKEN <<'EOF'
#include <iostream>

int main() {
    int a, b;
    std::cin >> a >> b;
    std::cout << a + b << " extra\n";
}
EOF

define CPP_NO_OUTPUT <<'EOF'
int main() {}
EOF

define CPP_COMPILE_ERROR <<'EOF'
int main() { this is not valid C++; }
EOF

define CPP_EXIT_1 <<'EOF'
int main() { return 1; }
EOF

define CPP_EXIT_125 <<'EOF'
int main() { return 125; }
EOF

define CPP_SEGFAULT <<'EOF'
int main() {
    volatile int* p = nullptr;
    *p = 1;
}
EOF

define CPP_DIVIDE_BY_ZERO <<'EOF'
#include <csignal>

int main() {
    raise(SIGFPE);
}
EOF

define CPP_UNCAUGHT_EXCEPTION <<'EOF'
int main() { throw 1; }
EOF

define CPP_BUSY_LOOP <<'EOF'
int main() {
    volatile unsigned long counter = 0;
    for (;;) counter = counter + 1;
}
EOF

define CPP_MEMORY_HOG <<'EOF'
#include <cstddef>
#include <vector>

int main() {
    const std::size_t n = 256ULL * 1024 * 1024;
    std::vector<char> big(n);
    volatile unsigned long sum = 0;
    for (std::size_t i = 0; i < n; i += 4096) sum = sum + big[i];
    return static_cast<int>(sum);
}
EOF

define CPP_ENDLESS_OUTPUT <<'EOF'
#include <cstdio>

int main() {
    for (;;) std::putchar('A');
}
EOF

define CPP_EXACTLY_64_MIB <<'EOF'
#include <iostream>

int main() {
    const int n = 64 * 1024 * 1024;
    for (int i = 0; i < n; ++i) std::cout.put(' ');
}
EOF

define CPP_CHECK_EMPTY_ENV <<'EOF'
#include <cstdlib>
#include <iostream>

int main() {
    const bool leaked = std::getenv("HOME") || std::getenv("PATH") || std::getenv("USER");
    std::cout << (leaked ? "leak" : "empty") << "\n";
}
EOF

define CPP_ORPHAN_CHILD <<'EOF'
#include <unistd.h>

int main() {
    if (fork() == 0) {
        for (;;) pause();
    }
    return 0;
}
EOF

define PY_SUM <<'EOF'
a, b = map(int, input().split())
print(a + b)
EOF

define PY_SUM_WRONG <<'EOF'
a, b = map(int, input().split())
print(a + b + 1)
EOF

define PY_SYNTAX_ERROR <<'EOF'
print(3
EOF

define PY_RUNTIME_ERROR <<'EOF'
raise ValueError("boom")
EOF

define PY_BUSY_LOOP <<'EOF'
while True:
    pass
EOF

define PY_SLEEP <<'EOF'
import time
time.sleep(10)
EOF

define PY_SLEEP_2S <<'EOF'
import time
time.sleep(2)
EOF

define PY_MEMORY_HOG <<'EOF'
data = bytearray(512 * 1024 * 1024)
EOF

define PY_ENDLESS_OUTPUT <<'EOF'
print('A' * 70 * 1024 * 1024)
EOF

# ---------------------------------------------------------------------------
# Preconditions
# ---------------------------------------------------------------------------

if ! curl -s -o /dev/null "$BASE_URL/problems"; then
    echo "Server is not reachable at $BASE_URL." >&2
    echo "Start it first:  OJ_DATA_DIR=/tmp/oj-test ./bin/OnlineJudge" >&2
    exit 1
fi

# ===========================================================================
section "API: problems"
# ===========================================================================

code=$(http POST /problems '{"title":"A+B"}')
assert_code "missing fields -> 400" 400 "$code"

code=$(http POST /problems 'not json')
assert_code "malformed JSON -> 400" 400 "$code"

code=$(http POST /problems '{"title":"x","statement":"y","time_limit":"fast","memory_limit":256}')
assert_code "non-numeric time_limit -> 400" 400 "$code"

code=$(http POST /problems '{"title":"","statement":"y","time_limit":1000,"memory_limit":256}')
assert_code "empty title -> 400" 400 "$code"

code=$(http POST /problems '{"title":"x","statement":"","time_limit":1000,"memory_limit":256}')
assert_code "empty statement -> 400" 400 "$code"

code=$(http POST /problems '{"title":"x","statement":"y","time_limit":0,"memory_limit":256}')
assert_code "time_limit = 0 -> 400" 400 "$code"

code=$(http POST /problems '{"title":"x","statement":"y","time_limit":1000,"memory_limit":-1}')
assert_code "negative memory_limit -> 400" 400 "$code"

code=$(http POST /problems '{"title":"x","statement":"y","time_limit":1000000,"memory_limit":256}')
assert_code "time_limit above the maximum -> 400" 400 "$code"

code=$(http POST /problems '{"title":"x","statement":"y","time_limit":1000,"memory_limit":1000000}')
assert_code "memory_limit above the maximum -> 400" 400 "$code"

LISTED_ID=$(create_problem "Listing check" 1000 256)
assert_match "valid problem is created and returns a numeric id" '^[0-9]+$' "$LISTED_ID"

code=$(http GET "/problems/$LISTED_ID")
assert_code "GET /problems/<id> -> 200" 200 "$code"
assert_eq "problem title round-trips" "Listing check" "$(json_get title)"
assert_eq "problem statement is returned" "Test problem" "$(json_get statement)"

code=$(http GET /problems)
assert_code "GET /problems -> 200" 200 "$code"
assert_eq "created problem appears in the list" "yes" "$(list_contains problem_id "$LISTED_ID")"

code=$(http GET /problems/999999)
assert_code "unknown problem -> 404" 404 "$code"

code=$(http GET /problems/not-a-number)
assert_code "non-numeric problem id -> 404" 404 "$code"

# ===========================================================================
section "API: test cases and transactional rollback"
# ===========================================================================

TX_ID=$(create_problem "Transactions" 1000 256)

code=$(http POST /test_cases '{"test_cases":[{"input":"a","output":"b"}]}')
assert_code "missing problem_id -> 400" 400 "$code"

code=$(http POST /test_cases '{"problem_id":"abc","test_cases":[{"input":"a","output":"b"}]}')
assert_code "non-integer problem_id -> 400" 400 "$code"

code=$(http POST /test_cases '{"problem_id":1.5,"test_cases":[{"input":"a","output":"b"}]}')
assert_code "fractional problem_id -> 400" 400 "$code"

code=$(http POST /test_cases '{"problem_id":999999,"test_cases":[{"input":"a","output":"b"}]}')
assert_code "unknown problem_id -> 404" 404 "$code"

code=$(http POST /test_cases "{\"problem_id\":$TX_ID}")
assert_code "missing test_cases array -> 400" 400 "$code"

code=$(http POST /test_cases "{\"problem_id\":$TX_ID,\"test_cases\":[]}")
assert_code "empty test_cases array -> 400" 400 "$code"

code=$(http POST /test_cases "{\"problem_id\":$TX_ID,\"test_cases\":[{\"input\":\"a\"}]}")
assert_code "test case without 'output' -> 400" 400 "$code"

code=$(http POST /test_cases \
    "{\"problem_id\":$TX_ID,\"test_cases\":[
        {\"input\":\"A\",\"output\":\"B\"},
        {\"input\":\"A\",\"output\":\"B\"}
    ]}")
assert_code "duplicate inside one batch -> 409" 409 "$code"

code=$(http POST /test_cases \
    "{\"problem_id\":$TX_ID,\"test_cases\":[{\"input\":\"A\",\"output\":\"B\"}]}")
assert_code "same case succeeds afterwards (proves the batch was rolled back)" 200 "$code"

code=$(http POST /test_cases \
    "{\"problem_id\":$TX_ID,\"test_cases\":[{\"input\":\"A\",\"output\":\"B\"}]}")
assert_code "duplicate of an already stored case -> 409" 409 "$code"

code=$(http POST /test_cases \
    "{\"problem_id\":$TX_ID,\"test_cases\":[
        {\"input\":\"C\",\"output\":\"D\"},
        {\"input\":\"A\",\"output\":\"B\"}
    ]}")
assert_code "batch with a valid + a duplicate case -> 409" 409 "$code"

code=$(http POST /test_cases \
    "{\"problem_id\":$TX_ID,\"test_cases\":[{\"input\":\"C\",\"output\":\"D\"}]}")
assert_code "valid case from the failed batch was not persisted -> 200" 200 "$code"

# ===========================================================================
section "API: submissions"
# ===========================================================================

SUBMIT_ID=$(create_problem "Submission validation" 1000 256)
add_test "$SUBMIT_ID" $'1 2\n' $'3\n'

code=$(http POST /submit 'not json')
assert_code "malformed JSON -> 400" 400 "$code"

code=$(http POST /submit '{"problem_id":1}')
assert_code "missing fields -> 400" 400 "$code"

code=$(http POST /submit '{"problem_id":"abc","language":"cpp","source_code":"int main(){}"}')
assert_code "non-integer problem_id -> 400" 400 "$code"

code=$(http POST /submit '{"problem_id":1.5,"language":"cpp","source_code":"int main(){}"}')
assert_code "fractional problem_id (submit) -> 400" 400 "$code"

code=$(http POST /submit '{"problem_id":999999,"language":"cpp","source_code":"int main(){}"}')
assert_code "unknown problem -> 404" 404 "$code"

code=$(http POST /submit "{\"problem_id\":$SUBMIT_ID,\"language\":\"brainf\",\"source_code\":\"+\"}")
assert_code "unsupported language -> 400" 400 "$code"
assert_contains "error message lists supported languages" "$(body)" "supported"

code=$(http POST /submit "{\"problem_id\":$SUBMIT_ID,\"language\":\"cpp\",\"source_code\":\"\"}")
assert_code "empty source code -> 400" 400 "$code"

python3 - "$WORK_DIR/oversized.json" "$SUBMIT_ID" <<'PY'
import json, sys

payload = {
    "problem_id": int(sys.argv[2]),
    "language": "cpp",
    "source_code": "a" * (64 * 1024 + 1),
}
with open(sys.argv[1], "w") as handle:
    json.dump(payload, handle)
PY
code=$(http POST /submit "@$WORK_DIR/oversized.json")
assert_code "source code over 64 KB -> 400" 400 "$code"

code=$(http GET /submissions/999999)
assert_code "unknown submission -> 404" 404 "$code"

# The language alias must be stored in its canonical form, and the full record
# must round-trip through GET /submissions/<id>.
SUB_ID=$(submit "$SUBMIT_ID" "c++" "$CPP_SUM")
assert_eq "alias 'c++' is accepted" "Accepted" "$(wait_for_verdict "$SUB_ID")"

code=$(http GET "/submissions/$SUB_ID")
assert_code "GET /submissions/<id> -> 200" 200 "$code"
assert_eq "language stored in canonical form" "cpp" "$(json_get language)"
assert_eq "problem_id is returned" "$SUBMIT_ID" "$(json_get problem_id)"
assert_contains "source code is returned" "$(json_get source_code)" "std::cin"
assert_match "execution_time_ms is a non-negative integer" '^[0-9]+$' "$(json_get execution_time_ms)"

code=$(http GET /submissions)
assert_code "GET /submissions -> 200" 200 "$code"
assert_eq "submission appears in the list" "yes" "$(list_contains submission_id "$SUB_ID")"

SUB_ID=$(submit "$SUBMIT_ID" "cpp17" "$CPP_SUM")
assert_eq "alias 'cpp17' is accepted" "Accepted" "$(wait_for_verdict "$SUB_ID")"

# ===========================================================================
section "Judge: basic verdicts"
# ===========================================================================

EMPTY_ID=$(create_problem "No test cases" 1000 256)
expect_verdict "problem without test cases" \
    "There are no test cases" "$EMPTY_ID" cpp "$CPP_NO_OUTPUT"

SUM_ID=$(create_problem "Sum" 1000 256)
add_test "$SUM_ID" $'1 2\n' $'3\n'

expect_verdict "C++ Accepted"                     "Accepted"                "$SUM_ID" cpp "$CPP_SUM"
expect_verdict "C++ Wrong Answer"                 "Wrong Answer on test 1"  "$SUM_ID" cpp "$CPP_SUM_WRONG"
expect_verdict "extra output token -> Wrong Answer" "Wrong Answer on test 1" "$SUM_ID" cpp "$CPP_SUM_EXTRA_TOKEN"
expect_verdict "missing output -> Wrong Answer"   "Wrong Answer on test 1"  "$SUM_ID" cpp "$CPP_NO_OUTPUT"
expect_verdict "odd whitespace is ignored"        "Accepted"                "$SUM_ID" cpp "$CPP_SUM_ODD_WHITESPACE"
expect_verdict "C++ Compilation Error"            "Compilation Error"       "$SUM_ID" cpp "$CPP_COMPILE_ERROR"

# ===========================================================================
section "Judge: runtime errors"
# ===========================================================================

SUM_ID=$(create_problem "Runtime errors" 1000 256)
add_test "$SUM_ID" $'1 2\n' $'3\n'

expect_verdict "non-zero exit code"               "Runtime Error on test 1" "$SUM_ID" cpp "$CPP_EXIT_1"
expect_verdict "exit code 125 is the user's, not ours" \
                                                  "Runtime Error on test 1" "$SUM_ID" cpp "$CPP_EXIT_125"
expect_verdict "segmentation fault"               "Runtime Error on test 1" "$SUM_ID" cpp "$CPP_SEGFAULT"
expect_verdict "division by zero (SIGFPE)"        "Runtime Error on test 1" "$SUM_ID" cpp "$CPP_DIVIDE_BY_ZERO"
expect_verdict "uncaught exception (SIGABRT)"     "Runtime Error on test 1" "$SUM_ID" cpp "$CPP_UNCAUGHT_EXCEPTION"
expect_verdict "Python exception"                 "Runtime Error on test 1" "$SUM_ID" python "$PY_RUNTIME_ERROR"

# ===========================================================================
section "Judge: multiple test cases"
# ===========================================================================

MULTI_ID=$(create_problem "Sum (3 tests)" 1000 256)

code=$(http POST /test_cases \
    "{\"problem_id\":$MULTI_ID,\"test_cases\":[
        {\"input\":\"1 2\\n\",\"output\":\"3\\n\"},
        {\"input\":\"10 20\\n\",\"output\":\"30\\n\"}
    ]}")
assert_code "batch of two test cases -> 200" 200 "$code"
add_test "$MULTI_ID" $'-5 5\n' $'0\n'

expect_verdict "all three tests pass"             "Accepted"                 "$MULTI_ID" cpp "$CPP_SUM"
expect_verdict "failure is reported on test 2"    "Wrong Answer on test 2"   "$MULTI_ID" cpp "$CPP_SUM_WRONG_ON_SECOND_TEST"
expect_verdict "crash is reported on test 3"      "Runtime Error on test 3"  "$MULTI_ID" cpp "$CPP_SUM_CRASH_ON_THIRD_TEST"

# ===========================================================================
section "Judge: time limit"
# ===========================================================================

TLE_ID=$(create_problem "TLE 200 ms" 200 256)
add_test "$TLE_ID" '' ''

expect_verdict "C++ busy loop"                    "Time Limit Exceeded on test 1" "$TLE_ID" cpp    "$CPP_BUSY_LOOP" 10
expect_verdict "Python busy loop"                 "Time Limit Exceeded on test 1" "$TLE_ID" python "$PY_BUSY_LOOP"  10
expect_verdict "sleeping program (wall-clock watchdog)" \
                                                  "Time Limit Exceeded on test 1" "$TLE_ID" python "$PY_SLEEP"      10

# A limit that is an exact multiple of one second sits right on the RLIMIT_CPU
# boundary, so it is tested separately.
TLE_1S_ID=$(create_problem "TLE 1000 ms" 1000 256)
add_test "$TLE_1S_ID" '' ''

expect_verdict "busy loop with a 1000 ms limit"   "Time Limit Exceeded on test 1" "$TLE_1S_ID" cpp "$CPP_BUSY_LOOP" 10

# ===========================================================================
section "Judge: memory limit"
# ===========================================================================

MLE_ID=$(create_problem "MLE 64 MB" 1000 64)
add_test "$MLE_ID" '' ''

expect_verdict "C++ allocates 256 MB under a 64 MB limit" \
                                                  "Memory Limit Exceeded on test 1" "$MLE_ID" cpp "$CPP_MEMORY_HOG" 10

PY_MLE_ID=$(create_problem "MLE 128 MB (Python)" 2000 128)
add_test "$PY_MLE_ID" '' ''

expect_verdict "Python MemoryError"               "Memory Limit Exceeded on test 1" "$PY_MLE_ID" python "$PY_MEMORY_HOG" 10

# ===========================================================================
section "Judge: output limit"
# ===========================================================================

OLE_ID=$(create_problem "Output limit" 5000 256)
add_test "$OLE_ID" '' ''

expect_verdict "C++ endless output"               "Output Limit Exceeded on test 1" "$OLE_ID" cpp    "$CPP_ENDLESS_OUTPUT" 20
expect_verdict "Python 70 MiB output"             "Output Limit Exceeded on test 1" "$OLE_ID" python "$PY_ENDLESS_OUTPUT"  20

BOUNDARY_ID=$(create_problem "Exact output boundary" 5000 256)
add_test "$BOUNDARY_ID" '' ''

# Exactly 64 MiB of whitespace is allowed and produces no comparison tokens.
expect_verdict "exactly 64 MiB of output is allowed" \
                                                  "Accepted" "$BOUNDARY_ID" cpp "$CPP_EXACTLY_64_MIB" 20

# ===========================================================================
section "Judge: Python"
# ===========================================================================

PY_ID=$(create_problem "Python sum" 2000 256)
add_test "$PY_ID" $'1 2\n' $'3\n'

expect_verdict "Python Accepted"                  "Accepted"                "$PY_ID" python  "$PY_SUM"
expect_verdict "alias 'python3'"                  "Accepted"                "$PY_ID" python3 "$PY_SUM"
expect_verdict "alias 'py' + Wrong Answer"        "Wrong Answer on test 1"  "$PY_ID" py      "$PY_SUM_WRONG"
expect_verdict "syntax error -> Compilation Error" "Compilation Error"      "$PY_ID" python  "$PY_SYNTAX_ERROR"

# ===========================================================================
section "Isolation of the child process"
# ===========================================================================

ENV_ID=$(create_problem "Empty environment" 1000 256)
add_test "$ENV_ID" '' $'empty\n'

expect_verdict "submission starts with an empty environment" \
                                                  "Accepted" "$ENV_ID" cpp "$CPP_CHECK_EMPTY_ENV"

ORPHAN_ID=$(create_problem "Orphan child" 1000 256)
add_test "$ORPHAN_ID" '' ''

expect_verdict "program that leaves a forked child behind still finishes" \
                                                  "Accepted" "$ORPHAN_ID" cpp "$CPP_ORPHAN_CHILD" 10

if [[ "$BASE_URL" =~ ^https?://(localhost|127\.0\.0\.1)(:|/|$) ]] && command -v pgrep >/dev/null 2>&1; then
    sleep 1
    if pgrep -f 'work_env/[0-9]+/solution' >/dev/null 2>&1; then
        fail "orphaned grandchild was killed with its process group" "a solution process is still running"
    else
        pass "orphaned grandchild was killed with its process group"
    fi
else
    skip "orphaned grandchild was killed with its process group" "needs a local server and pgrep"
fi

# ===========================================================================
section "Concurrency: stress test ($STRESS_COUNT simultaneous submissions)"
# ===========================================================================
#
# Fires STRESS_COUNT submissions at the same moment (one backgrounded curl each),
# alternating between a solution that must be Accepted and one that must get
# Wrong Answer. This exercises the bounded queue, the worker threads, the
# database mutex and the fork/exec/watchdog path under real contention.
#
# The expected verdict is decided by this script before the request is sent and
# is matched to the response through the submission_id that very request gets
# back, so the check does not depend on the order in which grading finishes.

STRESS_DIR="$WORK_DIR/stress"
mkdir -p "$STRESS_DIR"

STRESS_ID=$(create_problem "Stress" 2000 256)
add_test "$STRESS_ID" $'1 2\n' $'3\n'

write_submit_body "$STRESS_DIR/ac.json" "$STRESS_ID" cpp "$CPP_SUM"
write_submit_body "$STRESS_DIR/wa.json" "$STRESS_ID" cpp "$CPP_SUM_WRONG"

for i in $(seq 1 "$STRESS_COUNT"); do
    if (( i % 2 == 0 )); then
        request_body="$STRESS_DIR/ac.json"
        echo "Accepted" > "$STRESS_DIR/expected_$i.txt"
    else
        request_body="$STRESS_DIR/wa.json"
        echo "Wrong Answer on test 1" > "$STRESS_DIR/expected_$i.txt"
    fi

    (
        status=$(curl -s -o "$STRESS_DIR/resp_$i.json" -w '%{http_code}' \
            -X POST "$BASE_URL/submit" \
            -H 'Content-Type: application/json' \
            --data-binary "@$request_body")
        echo "$status" > "$STRESS_DIR/status_$i.txt"
    ) &
done

wait

STRESS_FAILURES=()
STRESS_IDS=()

for i in $(seq 1 "$STRESS_COUNT"); do
    status="$(cat "$STRESS_DIR/status_$i.txt" 2>/dev/null || echo none)"
    sub_id="$(json_get submission_id "$STRESS_DIR/resp_$i.json")"
    expected="$(cat "$STRESS_DIR/expected_$i.txt")"

    if [[ "$status" != "200" || -z "$sub_id" ]]; then
        STRESS_FAILURES+=("request $i: HTTP $status, no usable submission_id")
        continue
    fi

    STRESS_IDS+=("$sub_id")
    got="$(wait_for_verdict "$sub_id" 30)"

    if [[ "$got" != "$expected" ]]; then
        STRESS_FAILURES+=("request $i (submission $sub_id): expected '$expected', got '$got'")
    fi
done

if [[ ${#STRESS_FAILURES[@]} -eq 0 ]]; then
    pass "all $STRESS_COUNT concurrent submissions were graded correctly"
else
    fail "stress test" "${#STRESS_FAILURES[@]} mismatch(es)"
    for line in "${STRESS_FAILURES[@]}"; do
        echo "           - $line"
    done
fi

unique_ids=$(printf '%s\n' "${STRESS_IDS[@]}" | sort -u | wc -l | tr -d ' ')
assert_eq "every concurrent submission got a unique id" "${#STRESS_IDS[@]}" "$unique_ids"

# ===========================================================================
section "Concurrency: backpressure ($BACKPRESSURE_COUNT submissions, expect HTTP 503)"
# ===========================================================================
#
# Every submission sleeps for two seconds, so the 8 workers stay busy and the
# 256-slot queue fills up. Anything beyond workers + queue must be rejected
# with 503 instead of growing memory without bound.

BP_DIR="$WORK_DIR/backpressure"
mkdir -p "$BP_DIR"

BP_ID=$(create_problem "Backpressure" 5000 256)
add_test "$BP_ID" '' ''
write_submit_body "$BP_DIR/sleep.json" "$BP_ID" python "$PY_SLEEP_2S"

for i in $(seq 1 "$BACKPRESSURE_COUNT"); do
    (
        status=$(curl -s -o "$BP_DIR/resp_$i.json" -w '%{http_code}' \
            -X POST "$BASE_URL/submit" \
            -H 'Content-Type: application/json' \
            --data-binary "@$BP_DIR/sleep.json")
        echo "$status" > "$BP_DIR/status_$i.txt"
    ) &
done

wait

accepted=0
rejected=0
rejected_id=""

for i in $(seq 1 "$BACKPRESSURE_COUNT"); do
    status="$(cat "$BP_DIR/status_$i.txt" 2>/dev/null || echo none)"

    if [[ "$status" == "200" ]]; then
        accepted=$((accepted + 1))
    elif [[ "$status" == "503" ]]; then
        rejected=$((rejected + 1))
        rejected_id="$(json_get submission_id "$BP_DIR/resp_$i.json")"
    fi
done

echo "           accepted: $accepted, rejected with 503: $rejected"

if (( rejected > 0 )); then
    pass "server answered 503 when the queue was full"
else
    fail "server answered 503 when the queue was full" \
        "no 503 seen; raise BACKPRESSURE_COUNT above workers + queue size"
fi

if [[ -n "$rejected_id" ]]; then
    http GET "/submissions/$rejected_id" >/dev/null
    assert_eq "rejected submission is recorded as Rejected" \
        "Rejected - server busy, please resubmit" "$(json_get verdict)"
fi

echo "           (the accepted submissions are still draining; give the server a minute)"

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

echo
echo "-------------------------------------------"
printf '%sPASSED: %d%s   %sFAILED: %d%s   %sSKIPPED: %d%s\n' \
    "$C_GREEN" "$PASS" "$C_RESET" \
    "$C_RED" "$FAIL" "$C_RESET" \
    "$C_YELLOW" "$SKIP" "$C_RESET"
echo "-------------------------------------------"

if (( FAIL > 0 )); then
    echo "Failed tests:"
    for name in "${FAILED_TESTS[@]}"; do
        echo "  - $name"
    done
    exit 1
fi

exit 0

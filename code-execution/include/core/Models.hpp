#pragma once

#include <string>

struct TestCase {
    int test_id;
    std::string input_data;
    std::string expected_output;
};

struct ProblemSummary {
    int problem_id;
    std::string title;
    int time_limit; // milliseconds
    int memory_limit; // megabytes
};

struct ProblemInfo : ProblemSummary {
    std::string statement;
};

struct SubmissionSummary {
    int submission_id;
    int problem_id;
    std::string language;
    std::string verdict;
    int execution_time;
};

struct SubmissionInfo : SubmissionSummary {
    std::string source_code;
};

struct TestCaseInput {
    std::string input;
    std::string expected_output;
};

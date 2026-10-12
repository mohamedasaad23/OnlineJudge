cmake_minimum_required(VERSION 3.18)

project(OnlineJudge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/../bin")

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

include(FetchContent)

FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG v1.3.4
)

FetchContent_MakeAvailable(Crow)

set(CORE_SOURCES
    src/api/ApiHandler.cpp

    src/core/Compiler.cpp
    src/core/Evaluator.cpp
    src/core/Judge.cpp
    src/core/Language.cpp
    src/core/OutcomeClassifier.cpp
    src/core/ProcessExecutor.cpp
    src/core/Verdict.cpp

    src/db/DatabaseManager.cpp

    src/service/SubmissionService.cpp
)

add_library(core STATIC
    ${CORE_SOURCES}
)

target_include_directories(core
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

add_executable(
    OnlineJudge
    src/main.cpp
)

target_compile_options(
    core
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

target_compile_options(
    OnlineJudge
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
)

if(TARGET SQLite3::SQLite3)
    set(SQLITE3_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(SQLITE3_TARGET SQLite::SQLite3)
else()
    message(FATAL_ERROR
        "Could not find an SQLite3 imported target"
    )
endif()

target_link_libraries(
    core
    PUBLIC
        Threads::Threads
        ${SQLITE3_TARGET}
        Crow::Crow
)

target_link_libraries(
    OnlineJudge
    PRIVATE
        core
)
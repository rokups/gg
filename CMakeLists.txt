cmake_minimum_required(VERSION 3.24)

project(gg VERSION 0.1.0 LANGUAGES C CXX)

option(GG_BUILD_TESTS "Build gg tests" ON)
option(GG_COVERAGE "Instrument gg for line and branch coverage" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CPM.cmake)

CPMAddPackage(
  NAME libgit2
  GITHUB_REPOSITORY libgit2/libgit2
  GIT_TAG v1.9.6
  OPTIONS
    "BUILD_TESTS OFF"
    "BUILD_CLI OFF"
    "BUILD_EXAMPLES OFF"
    "BUILD_FUZZERS OFF"
    "USE_HTTPS OpenSSL-Dynamic"
)

CPMAddPackage(
  NAME CLI11
  GITHUB_REPOSITORY CLIUtils/CLI11
  GIT_TAG v2.6.2
  OPTIONS "CLI11_BUILD_TESTS OFF" "CLI11_BUILD_EXAMPLES OFF"
)

add_library(
  gg_core
  STATIC
  src/app.cpp
  src/cli.cpp
  src/cli_replay.cpp
  src/git_repository.cpp
  src/working_copy.cpp
  src/revisions.cpp
  src/rewrite.cpp
  src/operations.cpp
  src/conflicts.cpp
  src/commands_common.cpp
  src/commands_change.cpp
  src/commands_file.cpp
  src/commands_rewrite.cpp
  src/commands_remote.cpp
)
target_include_directories(gg_core PUBLIC include ${libgit2_SOURCE_DIR}/include)
target_link_libraries(gg_core PUBLIC libgit2package PRIVATE CLI11::CLI11)
target_compile_options(gg_core PRIVATE -Wall -Wextra -Wpedantic -Werror
                                       -Wno-missing-field-initializers)

add_executable(gg src/main.cpp)
target_link_libraries(gg PRIVATE gg_core)
target_compile_options(gg PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(GG_COVERAGE)
  target_compile_options(gg_core PUBLIC --coverage -O0 -g)
  target_link_options(gg_core PUBLIC --coverage)
endif()

if(GG_BUILD_TESTS)
  enable_testing()
  CPMAddPackage(
    NAME googletest
    GITHUB_REPOSITORY google/googletest
    GIT_TAG v1.17.0
    OPTIONS "INSTALL_GTEST OFF"
  )

  add_executable(
    gg_tests
    tests/app_test.cpp
    tests/workflow_test.cpp
    tests/file_test.cpp
    tests/rewrite_workflow_test.cpp
    tests/conflict_test.cpp
    tests/remote_test.cpp
    tests/internals_test.cpp
  )
  target_include_directories(gg_tests PRIVATE src)
  target_link_libraries(gg_tests PRIVATE gg_core GTest::gtest_main)
  target_compile_options(gg_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

  include(GoogleTest)
  gtest_discover_tests(gg_tests DISCOVERY_MODE PRE_TEST)
  add_test(NAME gg_help COMMAND gg --help)

  if(GG_COVERAGE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(
      coverage
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/coverage.py
              --build-dir ${CMAKE_BINARY_DIR}
              --source-dir ${CMAKE_SOURCE_DIR}
      DEPENDS gg gg_tests
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      USES_TERMINAL
    )
  endif()
endif()

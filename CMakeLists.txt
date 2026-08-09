cmake_minimum_required(VERSION 3.24)

project(gg VERSION 0.1.0 LANGUAGES C CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(GG_BUILD_TESTS "Build gg tests" ON)
option(GG_COVERAGE "Instrument gg for line and branch coverage" OFF)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

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
    "EXPERIMENTAL_SHA256 ON"
    "USE_HTTPS OpenSSL-Dynamic"
    "USE_SSH exec"
)

CPMAddPackage(
  NAME CLI11
  GITHUB_REPOSITORY CLIUtils/CLI11
  GIT_TAG v2.6.2
  OPTIONS "CLI11_BUILD_TESTS OFF" "CLI11_BUILD_EXAMPLES OFF"
)

add_library(
  gg_impl OBJECT
  src/git_repository.cpp
  src/working_copy.cpp
  src/revisions.cpp
  src/rewrite.cpp
  src/operations.cpp
  src/conflicts.cpp
  src/commands_common.cpp
  src/commands_change.cpp
  src/commands_diff.cpp
  src/commands_file.cpp
  src/commands_config.cpp
  src/commands_rewrite.cpp
  src/commands_remote.cpp
)
set_target_properties(
  gg_impl
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_include_directories(
  gg_impl
  PRIVATE
    include
    ${libgit2_BINARY_DIR}/include
    ${libgit2_SOURCE_DIR}/include
    src
)
target_compile_definitions(gg_impl PRIVATE GIT_EXPERIMENTAL_SHA256)
target_compile_options(gg_impl PRIVATE -Wall -Wextra -Wpedantic -Werror
                                       -Wno-missing-field-initializers)

add_library(gg_lib src/capi.cpp $<TARGET_OBJECTS:gg_impl>)
add_library(gg::gg ALIAS gg_lib)
set_target_properties(
  gg_lib
  PROPERTIES
    OUTPUT_NAME gg
    EXPORT_NAME gg
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
target_include_directories(
  gg_lib
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE src
)
target_include_directories(
  gg_lib SYSTEM
  PUBLIC
    $<BUILD_INTERFACE:${libgit2_BINARY_DIR}/include>
    $<BUILD_INTERFACE:${libgit2_SOURCE_DIR}/include>
)
target_link_libraries(
  gg_lib
  PUBLIC
    $<BUILD_INTERFACE:libgit2package>
    $<INSTALL_INTERFACE:libgit2::libgit2package>
)
if(NOT BUILD_SHARED_LIBS)
  target_link_libraries(gg_lib INTERFACE ${CMAKE_CXX_IMPLICIT_LINK_LIBRARIES})
endif()
target_compile_definitions(gg_lib PUBLIC GIT_EXPERIMENTAL_SHA256)
if(BUILD_SHARED_LIBS)
  target_compile_definitions(gg_lib PUBLIC GG_SHARED PRIVATE GG_BUILDING_LIBRARY)
endif()
target_compile_options(gg_lib PRIVATE -Wall -Wextra -Wpedantic -Werror
                                      -Wno-missing-field-initializers)

add_library(
  gg_cli_lib STATIC
  src/app.cpp
  src/cli.cpp
  src/cli_replay.cpp
  $<TARGET_OBJECTS:gg_impl>
)
target_include_directories(gg_cli_lib PUBLIC src)
target_link_libraries(gg_cli_lib PUBLIC gg_lib PRIVATE CLI11::CLI11)
target_compile_options(gg_cli_lib PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(gg_cli src/main.cpp)
set_target_properties(gg_cli PROPERTIES OUTPUT_NAME gg)
target_link_libraries(gg_cli PRIVATE gg_cli_lib)
target_compile_options(gg_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)

install(
  TARGETS gg_lib gg_cli
  EXPORT ggTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/gg DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_package_config_file(
  cmake/ggConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ggConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gg
)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/ggConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)
install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ggConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ggConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gg
)
install(
  EXPORT ggTargets
  NAMESPACE gg::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/gg
)

if(GG_COVERAGE)
  target_compile_options(gg_impl PUBLIC --coverage -O0 -g)
  target_link_options(gg_impl PUBLIC --coverage)
  target_compile_options(gg_lib PUBLIC --coverage -O0 -g)
  target_link_options(gg_lib PUBLIC --coverage)
  target_compile_options(gg_cli_lib PUBLIC --coverage -O0 -g)
  target_link_options(gg_cli_lib PUBLIC --coverage)
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
    tests/capi_workflow_test.cpp
    tests/commit_test.cpp
    tests/metaedit_test.cpp
    tests/workflow_test.cpp
    tests/git_compatibility_test.cpp
    tests/diff_test.cpp
    tests/file_test.cpp
    tests/config_test.cpp
    tests/rewrite_workflow_test.cpp
    tests/restore_test.cpp
    tests/simplify_parents_test.cpp
    tests/conflict_test.cpp
    tests/remote_test.cpp
    tests/tag_test.cpp
    tests/internals_test.cpp
  )
  target_include_directories(gg_tests PRIVATE src)
  target_link_libraries(gg_tests PRIVATE gg_cli_lib GTest::gtest_main)
  target_compile_options(gg_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)

  add_executable(gg_capi_test tests/capi_test.c)
  set_target_properties(gg_capi_test PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
  target_link_libraries(gg_capi_test PRIVATE gg_lib)
  target_compile_options(gg_capi_test PRIVATE -Wall -Wextra -Wpedantic -Werror)

  include(GoogleTest)
  gtest_discover_tests(
    gg_tests
    DISCOVERY_MODE PRE_TEST
  )
  add_test(NAME gg_help COMMAND gg_cli --help)
  add_test(NAME gg_capi COMMAND gg_capi_test)
  set_tests_properties(gg_capi PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set(GG_TEST_INSTALL_PREFIX ${CMAKE_BINARY_DIR}/install-test)
  add_test(
    NAME gg_install
    COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR}
            --prefix ${GG_TEST_INSTALL_PREFIX}
  )
  add_test(
    NAME gg_consumer_configure
    COMMAND ${CMAKE_COMMAND}
            -S ${CMAKE_SOURCE_DIR}/tests/consumer
            -B ${CMAKE_BINARY_DIR}/consumer-test
            -DCMAKE_PREFIX_PATH=${GG_TEST_INSTALL_PREFIX}
  )
  add_test(
    NAME gg_consumer_build
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/consumer-test
  )
  set_tests_properties(gg_consumer_configure PROPERTIES DEPENDS gg_install)
  set_tests_properties(gg_consumer_build PROPERTIES DEPENDS gg_consumer_configure)

  if(GG_COVERAGE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(
      coverage
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/coverage.py
              --build-dir ${CMAKE_BINARY_DIR}
              --source-dir ${CMAKE_SOURCE_DIR}
      DEPENDS gg_cli gg_tests
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      USES_TERMINAL
    )
  endif()
endif()

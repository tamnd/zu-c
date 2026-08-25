# The C and C++ kit for zu.
#
# What this builds is nothing: zu.hpp is a header, and a header-only
# library is an interface target and a directory to install. What it
# does is find the engine, run the suite against it, compile every
# example, and install the header with a config package so that
# find_package(zu-cpp) works downstream.
#
# The engine is a separate repository and is not built here. Point at
# one with ZU_ROOT, or let pkg-config find an installed one:
#
#   cmake -B build -DZU_ROOT=/path/to/zu
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# A checkout with no engine beside it configures and installs the
# header, and skips the suite, because the header is the artifact and it
# compiles against a header rather than against a library. A checkout
# where the engine was found and the suite is red is red.
cmake_minimum_required(VERSION 3.20)

# ZU_ROOT is the hint this project documents, and find_package only
# treats an upper case <PACKAGE>_ROOT as one under this policy. Without
# it a caller who set ZU_ROOT gets a warning saying it was ignored, and
# then it is found by FindZu anyway, which is a confusing pair of
# messages to hand somebody whose build worked.
if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

project(zu-cpp
  VERSION 0.15.0
  DESCRIPTION "The header-only C++ wrapper over libzu"
  HOMEPAGE_URL "https://github.com/tamnd/zu-c"
  LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# Everything below is for building this repository rather than for
# consuming it, so a project that pulls this in with add_subdirectory
# gets the header and nothing else.
set(ZU_CPP_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(ZU_CPP_TOP_LEVEL ON)
endif()

option(ZU_CPP_TESTS "Build the test suite" ${ZU_CPP_TOP_LEVEL})
option(ZU_CPP_EXAMPLES "Build the examples" ${ZU_CPP_TOP_LEVEL})
option(ZU_CPP_BENCH "Build the benchmarks" ${ZU_CPP_TOP_LEVEL})
# Off even at top level, unlike the three above. Doxygen is not needed
# to build this header or to use it, and a contributor who has a
# compiler and nothing else should be able to configure the repository
# and run the suite. CI turns it on, and so does the release.
option(ZU_CPP_DOCS "Build the API reference, which needs Doxygen" OFF)
option(ZU_CPP_WERROR "Treat warnings as errors in this repository's own code" OFF)
set(ZU_CPP_SANITIZE "" CACHE STRING "Sanitizers to build the suite with, for example address,undefined")

# ---- the library ----

add_library(zu_cpp INTERFACE)
add_library(zu::cpp ALIAS zu_cpp)

# So that the installed package spells it the same way the build tree
# does. Without this the export is zu::zu_cpp, and a project that
# followed the README links against a target that is not there.
set_target_properties(zu_cpp PROPERTIES EXPORT_NAME cpp)

target_include_directories(zu_cpp INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)

# C++20 is the floor. The std::expected half of the error model needs
# 23 and turns itself off below that, which is what ZU_HAS_EXPECTED
# reports, so a C++20 project gets the throwing API and compiles.
target_compile_features(zu_cpp INTERFACE cxx_std_20)

find_package(Zu QUIET)
if(TARGET zu::zu)
  target_link_libraries(zu_cpp INTERFACE zu::zu)
  message(STATUS "zu: ABI ${ZU_ABI_VERSION}, header ${ZU_INCLUDE_DIR}, library ${ZU_LIBRARY}")

  # The header is written against one revision of the ABI. A mismatch
  # is not fatal here, because the two numbers are counts and a header
  # newer than this wrapper is usually only wider, but it is worth
  # saying out loud rather than finding at the first struct.
  if(NOT ZU_ABI_VERSION VERSION_EQUAL "0.15")
    message(WARNING
      "zu.h declares ABI ${ZU_ABI_VERSION} and this wrapper was written against 0.15")
  endif()
else()
  message(STATUS
    "zu: no engine found, so the header installs and nothing runs. "
    "Set ZU_ROOT to a checkout or an SDK to run the suite.")
endif()

# ---- how this repository's own code is built ----

# Not on zu_cpp: these are how the tests, the examples and the
# benchmarks are compiled here, and forcing them on a consumer of an
# interface target would be forcing a warning policy on somebody else's
# build.
add_library(zu_cpp_build_settings INTERFACE)
if(MSVC)
  target_compile_options(zu_cpp_build_settings INTERFACE /W4 /permissive- /utf-8
    $<$<BOOL:${ZU_CPP_WERROR}>:/WX>)
else()
  target_compile_options(zu_cpp_build_settings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    $<$<BOOL:${ZU_CPP_WERROR}>:-Werror>)
endif()

if(ZU_CPP_SANITIZE)
  # Both flags, because a sanitizer that instruments the compile and not
  # the link is a sanitizer that does not run.
  target_compile_options(zu_cpp_build_settings INTERFACE
    -fsanitize=${ZU_CPP_SANITIZE} -fno-omit-frame-pointer -g)
  target_link_options(zu_cpp_build_settings INTERFACE -fsanitize=${ZU_CPP_SANITIZE})
endif()

# The engine is a dylib with an absolute install name in a cargo build
# tree and an @rpath one in a release archive, so the directory it was
# found in goes on the rpath of everything built here. Without it a
# release archive links and then fails to load.
if(ZU_LIBRARY_DIR)
  set(CMAKE_BUILD_RPATH "${ZU_LIBRARY_DIR}")
endif()

# ---- the suite, the examples, the benchmarks ----

# Here rather than beside add_subdirectory(test), because the reference
# is graded by ctest too and it needs no engine to be graded.
if(ZU_CPP_TESTS)
  enable_testing()
endif()

if(TARGET zu::zu AND ZU_CPP_TESTS)
  add_subdirectory(test)
  # The front page is code too, and it was the only code here that
  # nothing compiled. It is read out of README.md at configure time, so
  # it is part of the suite rather than a copy of the suite.
  add_subdirectory(readme)
endif()

if(TARGET zu::zu AND ZU_CPP_EXAMPLES)
  add_subdirectory(examples)
endif()

if(TARGET zu::zu AND ZU_CPP_BENCH)
  add_subdirectory(bench)
endif()

# No engine on this one. The reference is about the header, and the
# header is what this repository has whether an engine was found or not.
if(ZU_CPP_DOCS)
  add_subdirectory(docs)
endif()

# ---- installing ----

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(TARGETS zu_cpp EXPORT zu-cpp-targets)
install(EXPORT zu-cpp-targets
  FILE zu-cpp-targets.cmake
  NAMESPACE zu::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zu-cpp")

configure_package_config_file(
  cmake/zu-cpp-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/zu-cpp-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zu-cpp")

# Same as the ABI revision rather than a version of its own, because a
# wrapper is written against one ABI and a user asking for a wrapper is
# asking for the one that speaks the ABI they have.
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/zu-cpp-config-version.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMinorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/zu-cpp-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/zu-cpp-config-version.cmake"
  cmake/FindZu.cmake
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/zu-cpp")

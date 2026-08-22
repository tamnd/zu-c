# The C half of the suite, which is the half that can be wrong about
# the ABI. A C++ test reaches the ABI through the wrapper, so every
# promise the wrapper keeps for a caller is a promise the C++ suite
# cannot ask the ABI to keep: a destructor does not run twice, a
# zu::Connection has no null state to pass, a Result cannot outlive its
# Connection by accident. Those are the misuse cases, and they are
# reachable only from C.
#
# POSIX, because the harness makes and clears a temporary directory with
# mkdir and dirent. CI is Linux and macOS and this is where the tests
# run; the header itself is built by whatever a consumer has.
if(UNIX)
  add_executable(misuse misuse.c)
  target_link_libraries(misuse PRIVATE zu::zu zu_cpp_build_settings)
  target_include_directories(misuse PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(misuse PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME misuse COMMAND misuse)

  # And the rule kept rather than broken, which needs real threads and
  # is therefore the only file here that starts any. Separate from
  # misuse.c because the thread sanitizer and the address sanitizer
  # cannot be linked into one binary, so CI builds this tree twice and
  # runs a different one of the two each time.
  find_package(Threads REQUIRED)
  add_executable(threads threads.c)
  target_link_libraries(threads PRIVATE zu::zu zu_cpp_build_settings Threads::Threads)
  target_include_directories(threads PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(threads PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
  add_test(NAME threads COMMAND threads)
endif()

# One executable per file, which is what gives ctest a name per file and
# a failure a place to be. The harness is a header and there is nothing
# to fetch.
#
# Each file is also run under C++20 where the toolchain has C++23, since
# the header claims to compile under both and the half that turns itself
# off below 23 is the half a test would not otherwise reach.
set(ZU_CPP_TEST_FILES
  test_query.cpp
  test_errors.cpp
  test_conditions.cpp
  test_values.cpp
  test_statement.cpp
  test_transaction.cpp
  test_columns.cpp
  test_bulk.cpp
  test_frame.cpp
  test_progress.cpp
  test_expected.cpp
  test_arrow.cpp
  test_idiom.cpp)

foreach(file IN LISTS ZU_CPP_TEST_FILES)
  cmake_path(GET file STEM name)
  add_executable(${name} ${file})
  target_link_libraries(${name} PRIVATE zu::cpp zu_cpp_build_settings)
  target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  # Twenty three where it is there, because that is where std::expected
  # is and a suite that never compiles the try_ half tests half the API.
  target_compile_features(${name} PRIVATE cxx_std_23)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

# The same sources at the floor the header promises. A separate target
# rather than a separate directory, because the point is that these very
# files compile, minus the cases that ask for std::expected.
foreach(file IN LISTS ZU_CPP_TEST_FILES)
  cmake_path(GET file STEM name)
  add_executable(${name}_cxx20 ${file})
  target_link_libraries(${name}_cxx20 PRIVATE zu::cpp zu_cpp_build_settings)
  target_include_directories(${name}_cxx20 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(${name}_cxx20 PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(NAME ${name}_cxx20 COMMAND ${name}_cxx20)
endforeach()

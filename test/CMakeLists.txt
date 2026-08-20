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
  test_values.cpp
  test_statement.cpp
  test_transaction.cpp
  test_columns.cpp
  test_bulk.cpp
  test_frame.cpp
  test_progress.cpp
  test_expected.cpp
  test_arrow.cpp)

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

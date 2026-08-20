# Every example is also a test, because an example that compiles and
# does not run is documentation that lies. They take no arguments, they
# write only under the system temporary directory, and they clean up
# after themselves, so ctest runs the lot in any order.

# The C example is here to keep the plain ABI honest: this is the C and
# C++ kit, and the C half has to build with a C compiler and no C++
# runtime anywhere near it.
add_executable(example_hello_c hello.c)
target_link_libraries(example_hello_c PRIVATE zu::zu zu_cpp_build_settings)
set_target_properties(example_hello_c PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
  C_EXTENSIONS OFF)
add_test(NAME example_hello_c COMMAND example_hello_c)

set(ZU_CPP_EXAMPLE_FILES
  hello.cpp
  load.cpp
  parameters.cpp
  columns.cpp
  transaction.cpp
  errors.cpp
  frame.cpp
  progress.cpp
  expected.cpp)

# C++20 rather than 23, because the floor is what an example has to
# hold to. expected.cpp is the one that notices, and it says so at
# runtime rather than failing to build.
foreach(file IN LISTS ZU_CPP_EXAMPLE_FILES)
  cmake_path(GET file STEM name)
  add_executable(example_${name} ${file})
  target_link_libraries(example_${name} PRIVATE zu::cpp zu_cpp_build_settings)
  set_target_properties(example_${name} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
  add_test(NAME example_${name} COMMAND example_${name})
endforeach()

# And expected.cpp again at 23, which is the standard that has the half
# it is about. Building it twice is the only way to know that both
# arms of the ifdef are real code.
add_executable(example_expected_cxx23 expected.cpp)
target_link_libraries(example_expected_cxx23 PRIVATE zu::cpp zu_cpp_build_settings)
target_compile_features(example_expected_cxx23 PRIVATE cxx_std_23)
add_test(NAME example_expected_cxx23 COMMAND example_expected_cxx23)

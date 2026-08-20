# The benchmarks are built at whatever the compiler defaults to for
# release and are not run by ctest, because a timing number produced on
# a machine that is also running a compile is not a number. Build them
# and run them by hand:
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release -DZU_ROOT=/path/to/zu
#   cmake --build build --target bench
#   ./build/bench/bench_read
#
# The bench target builds all of them and runs none, which is the one
# thing CI should do with them: a benchmark that stops compiling is a
# benchmark that stops being run.

set(ZU_CPP_BENCH_FILES
  bench_query.cpp
  bench_read.cpp
  bench_write.cpp
  bench_frame.cpp)

add_custom_target(bench)

foreach(file IN LISTS ZU_CPP_BENCH_FILES)
  cmake_path(GET file STEM name)
  add_executable(${name} EXCLUDE_FROM_ALL ${file})
  target_link_libraries(${name} PRIVATE zu::cpp zu_cpp_build_settings)
  target_compile_features(${name} PRIVATE cxx_std_20)
  # A benchmark of a debug build measures the debug build. Saying so at
  # configure time is cheaper than working it out from the numbers.
  if(NOT CMAKE_BUILD_TYPE MATCHES "Rel")
    set_property(TARGET ${name} APPEND PROPERTY
      COMPILE_DEFINITIONS ZU_BENCH_UNOPTIMIZED=1)
  endif()
  add_dependencies(bench ${name})
endforeach()

if(NOT CMAKE_BUILD_TYPE MATCHES "Rel")
  message(STATUS
    "zu: benchmarks will be built without optimization, "
    "because CMAKE_BUILD_TYPE is '${CMAKE_BUILD_TYPE}'")
endif()

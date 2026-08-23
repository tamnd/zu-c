# The reference, and the test that it is one.
#
# Off by default, because Doxygen is not a dependency of building or
# using this header and a contributor with a compiler and nothing else
# should be able to configure this repository. On in CI, where the
# release builds what it publishes.
#
#   cmake -B build -DZU_CPP_DOCS=ON
#   cmake --build build --target docs
#   ctest --test-dir build -R reference
#
# No engine is needed for any of it. The reference is about the header,
# and the header is what this repository owns. zu.h is generated in the
# engine's tree and is not here, so a reference for the C API belongs
# where the C API is written rather than as a second copy that drifts
# the day the generator runs again.

find_package(Doxygen REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Absolute, because Doxygen resolves what it is given against whatever
# directory it was started in and reference.py reads these back out of
# the configured file to find out where the reference went.
set(ZU_CPP_DOCS_INPUT "${CMAKE_SOURCE_DIR}/include/zu.hpp")
set(ZU_CPP_DOCS_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/reference")
# Beside the output and not in it, because the output directory is
# emptied on every run and the log would go with it.
set(ZU_CPP_DOCS_WARNINGS "${CMAKE_CURRENT_BINARY_DIR}/warnings.txt")

set(ZU_CPP_DOXYFILE "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile")
configure_file(Doxyfile.in "${ZU_CPP_DOXYFILE}" @ONLY)

# Through reference.py rather than through Doxygen, everywhere. Doxygen
# exits 0 on a header it read nothing out of, so running it on its own
# is running the half of this that cannot fail.
set(ZU_CPP_REFERENCE
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/reference.py" "${ZU_CPP_DOXYFILE}")

add_custom_target(docs
  COMMAND ${ZU_CPP_REFERENCE}
  DEPENDS "${ZU_CPP_DOCS_INPUT}" "${ZU_CPP_DOXYFILE}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  COMMENT "Generating the reference from include/zu.hpp"
  VERBATIM)

# The same command as the target, so that what CI publishes and what
# ctest grades are one run of one script and not two things that agree
# until they do not.
if(ZU_CPP_TESTS)
  add_test(NAME reference COMMAND ${ZU_CPP_REFERENCE})
endif()

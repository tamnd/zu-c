# The reference, the published surface, and the tests that they are
# what they say.
#
# Off by default, because Doxygen is not a dependency of building or
# using this header and a contributor with a compiler and nothing else
# should be able to configure this repository. On in CI, where the
# release builds what it publishes.
#
#   cmake -B build -DZU_CPP_DOCS=ON
#   cmake --build build --target docs
#   ctest --test-dir build -R 'reference|surface'
#
# Both gates read one Doxygen run: the reference asks whether the
# header was documented, and the surface asks whether what it publishes
# is still what api/surface.txt says. When the answer to the second is
# no on purpose,
#
#   cmake --build build --target surface-update
#
# writes the file again for review.
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

set(ZU_CPP_SURFACE
  "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/surface.py" "${ZU_CPP_DOXYFILE}")

# Not part of `docs`, and not a test either. Rewriting a reviewed file
# is what a person asks for when they meant to move the surface, and a
# build that did it on the way past would turn the gate into a step
# that always passes.
add_custom_target(surface-update
  COMMAND ${ZU_CPP_SURFACE} --update
  DEPENDS "${ZU_CPP_DOCS_INPUT}" "${ZU_CPP_DOXYFILE}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  COMMENT "Writing api/surface.txt from include/zu.hpp"
  VERBATIM)

# The same commands as the targets, so that what CI publishes and what
# ctest grades are one run of one script and not two things that agree
# until they do not.
if(ZU_CPP_TESTS)
  add_test(NAME reference COMMAND ${ZU_CPP_REFERENCE})
  add_test(NAME surface COMMAND ${ZU_CPP_SURFACE})

  # Both empty the reference directory and run Doxygen into it, so they
  # are two tests over one piece of ground and ctest -j would have them
  # deleting each other's output. The lock is what says so; the
  # alternative, one test doing both jobs, would mean a header that is
  # undocumented and a surface that moved arrive as one failure.
  set_tests_properties(reference surface PROPERTIES RESOURCE_LOCK doxygen)

  # And the rules the surface gate rests on, which are judgements about
  # which half of a C++ declaration is its identity. Getting one of them
  # wrong does not make the gate fail, it makes it pass, so they are
  # checked rather than argued. No Doxygen and no header in these: they
  # run on XML written out by hand.
  add_test(NAME surface-cases
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover
            -s "${CMAKE_CURRENT_SOURCE_DIR}" -p "test_*.py")
endif()

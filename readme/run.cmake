# One of the README's programs, run in a directory of its own, and what
# it printed compared with what the page says it prints.
#
# A cmake script rather than a shell line because this suite runs on
# Linux, macOS and Windows, and a comparison that is a pipeline on two of
# them and something else on the third is a comparison that will differ
# on the third. A directory of its own because these programs create a
# database and refuse a path that already holds one, so a run that
# inherited the last run's file would fail for a reason that has nothing
# to do with the page.

if(NOT PROGRAM OR NOT EXPECTED OR NOT WORK)
  message(FATAL_ERROR "run.cmake wants PROGRAM, EXPECTED and WORK")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

execute_process(
  COMMAND "${PROGRAM}"
  WORKING_DIRECTORY "${WORK}"
  OUTPUT_VARIABLE printed
  ERROR_VARIABLE complained
  RESULT_VARIABLE status)

# NOTICE for the quoted text and FATAL_ERROR for the sentence, because
# FATAL_ERROR indents and re-wraps what it is given and a diff that has
# been re-wrapped is not a diff. NOTICE prints what it was handed.
if(NOT status EQUAL 0)
  message(NOTICE "${complained}")
  message(FATAL_ERROR
    "the README's program exited ${status}, and the page prints it as one that works")
endif()

file(READ "${EXPECTED}" expected)
if(NOT printed STREQUAL expected)
  message(NOTICE "the README's program printed:")
  message(NOTICE "${printed}")
  message(NOTICE "and the block under it on the page says it prints:")
  message(NOTICE "${expected}")
  message(FATAL_ERROR "the page and the program it prints do not agree")
endif()

# Anything on the error stream from a program the page prints as working
# is worth seeing, even when it exited zero and printed the right rows.
if(complained)
  message(STATUS "it also said, on the error stream: ${complained}")
endif()

if(LEAVES AND NOT EXISTS "${WORK}/${LEAVES}")
  message(FATAL_ERROR
    "the page says the program writes ${LEAVES} in the directory you run it from, and it did not")
endif()

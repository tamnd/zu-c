# The README's programs, built and run as printed.
#
# A quickstart is the most read and least executed code a client has. It
# is copied out of a page by hand, and it goes wrong quietly, a rename at
# a time, until somebody's first five minutes are spent on a call that no
# longer exists. Both programs on that page were wrong when this
# directory was written, and in the same way: each opened a database that
# nothing had created, so neither would have run for anybody who tried
# it. Nothing in the suite could have noticed, because the page was the
# one piece of code in this repository that was never compiled.
#
# The rule the page follows is that a block with an `int main` in it is a
# whole program and everything else is a fragment, and that the block
# under a program, fenced with no language, is what it prints. So a
# program here is built, run in a directory of its own, and diffed
# against that block. Nothing below is copied off the page by hand: the
# source and the expected output are both read out of README.md at
# configure time, which is what makes a rename on the page a red suite
# rather than a page that has quietly stopped being true.

set(readme "${CMAKE_CURRENT_SOURCE_DIR}/../README.md")

# So that editing the page re-runs cmake. Without this the blocks below
# are extracted once and then held, and the test goes on passing against
# a program the README no longer prints.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${readme}")

file(READ "${readme}" page)

set(blocks "${CMAKE_CURRENT_BINARY_DIR}/blocks")
file(REMOVE_RECURSE "${blocks}")
file(MAKE_DIRECTORY "${blocks}")

# Split by consuming the page from the front rather than by one regular
# expression over the whole of it. A C program is mostly semicolons, and
# a semicolon is a list separator here; and CMake's regular expressions
# are greedy with no way to ask them not to be, so a pattern for one
# fenced block matches from the first fence to the last. Walking it is
# duller and it is right.
set(rest "\n${page}")
set(languages "")
set(count 0)
while(TRUE)
  string(FIND "${rest}" "\n```" open)
  if(open LESS 0)
    break()
  endif()
  math(EXPR open "${open} + 4")
  string(SUBSTRING "${rest}" ${open} -1 rest)

  # The rest of the fence line is the language, and no language at all
  # is what the page uses for a block of output.
  string(FIND "${rest}" "\n" eol)
  string(SUBSTRING "${rest}" 0 ${eol} language)
  math(EXPR eol "${eol} + 1")
  string(SUBSTRING "${rest}" ${eol} -1 rest)
  string(STRIP "${language}" language)
  if(language STREQUAL "")
    set(language "text")
  endif()

  string(FIND "${rest}" "\n```" close)
  if(close LESS 0)
    message(FATAL_ERROR "the README has a fenced block it never closes")
  endif()
  string(SUBSTRING "${rest}" 0 ${close} body)
  math(EXPR close "${close} + 4")
  string(SUBSTRING "${rest}" ${close} -1 rest)

  # The trailing newline the closing fence sat on belongs to the block,
  # because a program's last line ends and because what a program prints
  # ends in one too, and the comparison below is exact.
  file(WRITE "${blocks}/${count}.txt" "${body}\n")
  list(APPEND languages "${language}")
  math(EXPR count "${count} + 1")
endwhile()

if(count LESS 2)
  message(FATAL_ERROR "the README has ${count} fenced blocks, so this is reading the wrong file")
endif()

math(EXPR last "${count} - 1")
set(programs 0)
foreach(i RANGE ${last})
  list(GET languages ${i} language)
  if(NOT language MATCHES "^(c|cpp)$")
    continue()
  endif()
  file(READ "${blocks}/${i}.txt" body)
  if(NOT body MATCHES "int main")
    continue()
  endif()

  math(EXPR next "${i} + 1")
  set(after "")
  if(next LESS_EQUAL ${last})
    list(GET languages ${next} after)
  endif()
  if(NOT after STREQUAL "text")
    message(FATAL_ERROR
      "block ${i} of the README is a whole ${language} program and nothing under it "
      "says what it prints, so there is nothing to check it against")
  endif()

  set(name readme_${language}_${programs})
  set(source "${CMAKE_CURRENT_BINARY_DIR}/${name}.${language}")
  configure_file("${blocks}/${i}.txt" "${source}" COPYONLY)

  add_executable(${name} "${source}")
  # The same warnings as everything else here, -Werror included where CI
  # turns it on. A page that only compiles when the reader is not
  # watching for warnings is a page that will teach one.
  if(language STREQUAL "c")
    target_link_libraries(${name} PRIVATE zu::zu zu_cpp_build_settings)
    set_target_properties(${name} PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF)
  else()
    target_link_libraries(${name} PRIVATE zu::cpp zu_cpp_build_settings)
    set_target_properties(${name} PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS OFF)
  endif()

  # The page says these programs write a database beside the reader, so
  # the name is read back out of the program rather than written down
  # here, and the run checks the file is there afterwards. A quickstart
  # that prints the right rows out of a database it did not leave behind
  # has told the reader something untrue about their own directory.
  set(leaves "")
  if(body MATCHES "\"([A-Za-z0-9_.-]+\\.zu1)\"")
    set(leaves "${CMAKE_MATCH_1}")
  endif()

  add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}"
    "-DPROGRAM=$<TARGET_FILE:${name}>"
    "-DEXPECTED=${blocks}/${next}.txt"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/run/${name}"
    "-DLEAVES=${leaves}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/run.cmake")

  math(EXPR programs "${programs} + 1")
endforeach()

# The negative case, which is the one that matters. A directory that
# found no programs adds no tests, and a suite with no tests in it is
# green: exactly the shape this is here to prevent, and the shape a
# retitled fence or a moved README would leave behind.
if(programs EQUAL 0)
  message(FATAL_ERROR
    "the README has no whole program in it, so this directory is checking nothing")
endif()
message(STATUS "readme: ${programs} whole programs of ${count} fenced blocks")

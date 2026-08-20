# Finds the engine, which is zu.h and libzu.
#
# Neither lives here. The header is generated from the engine's API
# model and ships with the library from tamnd/zu, so this looks for what
# a user already has rather than building anything.
#
# Where it looks, in order, first hit wins:
#
#   ZU_ROOT         a directory with include/zu.h and lib/libzu under it,
#                   which is what a release archive unpacks to. Also read
#                   from the environment, since that is where a shell
#                   that already sourced an SDK keeps it.
#   ZU_INCLUDE_DIR  the directory zu.h is in, and
#   ZU_LIBRARY      the library file itself, for pointing straight at a
#                   build tree without laying an SDK out.
#   pkg-config zu   what `cargo xtask package` writes a zu.pc for.
#   the usual places
#
# An engine checkout is understood as a root too: crates/zu-capi/include
# is where the header is in one and target/release is where cargo leaves
# the library, so ZU_ROOT=/path/to/zu works on a checkout that has been
# built.
#
# What it defines:
#
#   zu::zu          an imported target with the include directory on it
#   Zu_FOUND
#   ZU_ABI_VERSION  the revision the header declares, read out of it, so
#                   that a caller can refuse a header it was not written
#                   against rather than find out at the first struct.

include(FindPackageHandleStandardArgs)

set(_zu_roots)
foreach(_hint IN ITEMS "${ZU_ROOT}" "$ENV{ZU_ROOT}")
  if(_hint)
    list(APPEND _zu_roots "${_hint}")
  endif()
endforeach()

set(_zu_include_hints)
set(_zu_library_hints)
foreach(_root IN LISTS _zu_roots)
  list(APPEND _zu_include_hints "${_root}/include" "${_root}/crates/zu-capi/include")
  list(APPEND _zu_library_hints "${_root}/lib" "${_root}/target/release")
endforeach()

if(NOT ZU_INCLUDE_DIR AND DEFINED ENV{ZU_INCLUDE_DIR})
  set(ZU_INCLUDE_DIR "$ENV{ZU_INCLUDE_DIR}")
endif()
if(NOT ZU_LIBRARY AND DEFINED ENV{ZU_LIBRARY})
  set(ZU_LIBRARY "$ENV{ZU_LIBRARY}")
endif()

# pkg-config last among the hints and never as an override, because a
# stale zu.pc on a machine where somebody is testing a build tree is
# exactly the case ZU_ROOT is for.
if(NOT ZU_INCLUDE_DIR OR NOT ZU_LIBRARY)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(_zu_pc QUIET zu)
    if(_zu_pc_FOUND)
      list(APPEND _zu_include_hints ${_zu_pc_INCLUDE_DIRS})
      list(APPEND _zu_library_hints ${_zu_pc_LIBRARY_DIRS})
    endif()
  endif()
endif()

find_path(ZU_INCLUDE_DIR
  NAMES zu.h
  HINTS ${_zu_include_hints}
  DOC "The directory zu.h is in")

find_library(ZU_LIBRARY
  NAMES zu libzu
  HINTS ${_zu_library_hints}
  DOC "The zu engine library")

if(ZU_INCLUDE_DIR AND EXISTS "${ZU_INCLUDE_DIR}/zu.h")
  file(STRINGS "${ZU_INCLUDE_DIR}/zu.h" _zu_abi_line
    REGEX "^#define[ \t]+ZU_ABI_VERSION[ \t]+\"")
  string(REGEX REPLACE "^.*\"([^\"]+)\".*$" "\\1" ZU_ABI_VERSION "${_zu_abi_line}")
  unset(_zu_abi_line)
endif()

find_package_handle_standard_args(Zu
  REQUIRED_VARS ZU_LIBRARY ZU_INCLUDE_DIR
  VERSION_VAR ZU_ABI_VERSION)

if(Zu_FOUND AND NOT TARGET zu::zu)
  # UNKNOWN rather than SHARED, because the same find turns up the
  # static library on a machine that only has that one and a static zu
  # links exactly as well.
  add_library(zu::zu UNKNOWN IMPORTED)
  set_target_properties(zu::zu PROPERTIES
    IMPORTED_LOCATION "${ZU_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ZU_INCLUDE_DIR}")
  get_filename_component(ZU_LIBRARY_DIR "${ZU_LIBRARY}" DIRECTORY)
endif()

mark_as_advanced(ZU_INCLUDE_DIR ZU_LIBRARY)
unset(_zu_roots)
unset(_zu_include_hints)
unset(_zu_library_hints)

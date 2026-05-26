# grimvault_detect_version (<out_var>)
#
# Resolves the project version from (in order):
#    1. $ENV{GRIMVAULT_VERSION}                  (CI override)
#    2. `git describe --tags --abbrev=0` of HEAD (stripped of leading v)
#    3. 0.0.0                                    (fallback)
#
function (grimvault_detect_version out_var)
   if (DEFINED ENV{GRIMVAULT_VERSION} AND NOT "$ENV{GRIMVAULT_VERSION}" STREQUAL "")
      set (${out_var} "$ENV{GRIMVAULT_VERSION}" PARENT_SCOPE)
      return ()
   endif ()

   find_package (Git QUIET)

   if (Git_FOUND)
      execute_process (
         COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
         OUTPUT_VARIABLE git_tag
         OUTPUT_STRIP_TRAILING_WHITESPACE
         ERROR_QUIET
         RESULT_VARIABLE git_rc
      )

      if (git_rc EQUAL 0 AND git_tag)
         string (REGEX REPLACE "^v" "" git_tag "${git_tag}")

         if (git_tag MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
            set (${out_var} "${git_tag}" PARENT_SCOPE)
            return ()
         endif ()
      endif ()
   endif ()

   set (${out_var} "0.0.0" PARENT_SCOPE)
endfunction ()


# grimvault_write_version_header (<path> <version>)
#
# Writes a header exposing the version as compile-time constants under
# gv::core::version.
#
function (grimvault_write_version_header path version)
   string (REPLACE "." ";" parts "${version}")
   list (LENGTH parts n)

   if (NOT n EQUAL 3)
      message (FATAL_ERROR "Invalid version '${version}'; expected MAJOR.MINOR.PATCH")
   endif ()

   list (GET parts 0 major)
   list (GET parts 1 minor)
   list (GET parts 2 patch)

   set (template
[=[
#pragma once

namespace gv::core::version {

constexpr int         major  = @major@;
constexpr int         minor  = @minor@;
constexpr int         patch  = @patch@;
constexpr const char* string = "@version@";

} // namespace gv::core::version
]=])

   string (CONFIGURE "${template}" rendered @ONLY)

   file (WRITE "${path}.tmp" "${rendered}")
   configure_file ("${path}.tmp" "${path}" COPYONLY)
   file (REMOVE "${path}.tmp")
endfunction ()

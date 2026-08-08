# grimvault_detect_version (<numeric_out> <display_out>)
#
# Resolves the project version from (in order):
#    1. -DGRIMVAULT_VERSION or $ENV{GRIMVAULT_VERSION} (CI override)
#    2. an exact SemVer tag on HEAD
#    3. 0.0.0-dev
#
function (grimvault_detect_version numeric_out display_out)
   set (raw "")

   if (DEFINED GRIMVAULT_VERSION AND NOT "${GRIMVAULT_VERSION}" STREQUAL "")
      set (raw "${GRIMVAULT_VERSION}")
   elseif (DEFINED ENV{GRIMVAULT_VERSION} AND NOT "$ENV{GRIMVAULT_VERSION}" STREQUAL "")
      set (raw "$ENV{GRIMVAULT_VERSION}")
   endif ()

   find_package (Git QUIET)

   if (NOT raw AND Git_FOUND)
      execute_process (
         COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match
         WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
         OUTPUT_VARIABLE git_tag
         OUTPUT_STRIP_TRAILING_WHITESPACE
         ERROR_QUIET
         RESULT_VARIABLE git_rc
      )

      if (git_rc EQUAL 0 AND git_tag)
         set (raw "${git_tag}")
      endif ()
   endif ()

   if (NOT raw)
      set (raw "0.0.0-dev")
   endif ()

   string (REGEX REPLACE "^v" "" display "${raw}")
   if (NOT display MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
      message (FATAL_ERROR "Invalid GrimVault version '${raw}'")
   endif ()

   string (REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" numeric "${display}")
   set (${numeric_out} "${numeric}" PARENT_SCOPE)
   set (${display_out} "${display}" PARENT_SCOPE)
endfunction ()


# grimvault_write_version_header (<path> <numeric_version> <display_version>)
#
# Writes a header exposing the version as compile-time constants under
# gv::core::version.
#
function (grimvault_write_version_header path version display_version)
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
constexpr const char* string = "@display_version@";

} // namespace gv::core::version
]=])

   string (CONFIGURE "${template}" rendered @ONLY)

   file (WRITE "${path}.tmp" "${rendered}")
   configure_file ("${path}.tmp" "${path}" COPYONLY)
   file (REMOVE "${path}.tmp")
endfunction ()

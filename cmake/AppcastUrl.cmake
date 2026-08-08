# grimvault_write_appcast_header (<path>)
#
# Writes <path>/gv/update/appcast_url.h with constexpr URL + ed25519 public
# key. The URL is env-aware: dev / qa / prod each get their own appcast feed
# so a dev binary cannot self-update to a prod build. Mirrors the per-env
# pattern in cmake/Env.cmake so the two stay in lockstep.
#
# CI overrides via -DGRIMVAULT_APPCAST_URL=... still win; the env table
# below is the default.

set (GRIMVAULT_APPCAST_PUBKEY "zqNoGIvc3ZHM3bzq59W7hUkseoIODaMd/nsXn42sXRU="
   CACHE STRING "Base64 ed25519 public key (32 bytes) for appcast enclosure signatures")

# Per-env appcast feed. Empty string for dev disables self-update entirely
# (the UpdateService treats an empty URL as "don't check").
if (NOT DEFINED GRIMVAULT_ENV)
   set (GRIMVAULT_ENV "prod")
endif ()
if (NOT DEFINED GRIMVAULT_CHANNEL)
   set (GRIMVAULT_CHANNEL "stable")
endif ()
if (NOT GRIMVAULT_CHANNEL MATCHES "^(stable|beta)$")
   message (FATAL_ERROR "GRIMVAULT_CHANNEL must be stable or beta")
endif ()

set (_GV_APPCAST_FILE "appcast.xml")
if (GRIMVAULT_CHANNEL STREQUAL "beta")
   set (_GV_APPCAST_FILE "appcast-beta.xml")
endif ()

if (GRIMVAULT_ENV STREQUAL "dev")
   set (_GV_APPCAST_DEFAULT "")
elseif (GRIMVAULT_ENV STREQUAL "qa")
   set (_GV_APPCAST_DEFAULT "https://releases.katforge.com/grimvault/qa/${_GV_APPCAST_FILE}")
else ()
   set (_GV_APPCAST_DEFAULT "https://releases.katforge.com/grimvault/${_GV_APPCAST_FILE}")
endif ()

set (GRIMVAULT_APPCAST_URL "${_GV_APPCAST_DEFAULT}"
   CACHE STRING "Appcast feed URL baked into the binary (empty disables self-update)")

function (grimvault_write_appcast_header out_path)
   set (template
[=[
#pragma once

namespace gv::update {

constexpr const char* appcast_url    = "@GRIMVAULT_APPCAST_URL@";
constexpr const char* appcast_pubkey = "@GRIMVAULT_APPCAST_PUBKEY@";

} // namespace gv::update
]=])

   string (CONFIGURE "${template}" rendered @ONLY)

   file (WRITE "${out_path}.tmp" "${rendered}")
   configure_file ("${out_path}.tmp" "${out_path}" COPYONLY)
   file (REMOVE "${out_path}.tmp")
endfunction ()

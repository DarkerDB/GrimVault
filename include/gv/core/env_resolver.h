#pragma once

#include <gv/core/env.h>

#include <string>
#include <string_view>
#include <vector>

namespace gv::core {

// Returns the EnvDef with matching name, or k_env_default when name is
// empty or unrecognized. Pure.
const EnvDef& env_for_name (std::string_view name);

// Resolves the active env for development and QA builds from (in priority order):
//    1. `--env <name>` in argv (consumed; the flag is dropped from argv)
//    2. GRIMVAULT_ENV environment variable
//    3. k_env_default (the compile-time default)
//
// Production builds always return k_env_default. `argv` is mutated to strip
// `--env` and its value when present so downstream parsing remains stable.
const EnvDef& resolve_active_env (std::vector<std::string>& argv);

// Read-only variant. Production builds always return k_env_default;
// development and QA builds read GRIMVAULT_ENV before using the default.
const EnvDef& resolve_active_env ();

// Process-wide active env. Set once early in main () after resolution; read
// from anywhere via active_env (). Defaults to k_env_default until set.
void           set_active_env (const EnvDef& env);
const EnvDef&  active_env     ();

} // namespace gv::core

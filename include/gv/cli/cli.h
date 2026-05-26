#pragma once

#include <string>
#include <vector>

namespace gv::cli {

// Exit codes — sysexits.h-ish (per contract §8).
enum : int {
   k_ok               = 0,
   k_error_generic    = 1,
   k_error_usage      = 2,
   k_error_auth       = 64,
   k_error_service    = 69,
   k_error_transient  = 75,
};

// Entry point for CLI mode. `args` is argv[1..] (subcommand + flags).
// Prints to stdout/stderr; returns an exit code.
int run (const std::vector<std::string>& args);

} // namespace gv::cli

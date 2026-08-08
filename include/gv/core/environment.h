#pragma once

#include <string>

namespace gv::core::environment {

// Reads a process environment variable without MSVC's unsafe-CRT warning.
// Missing variables and variables with empty values both return an empty string.
std::string get (const char* name);

} // namespace gv::core::environment

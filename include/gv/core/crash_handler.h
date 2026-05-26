#pragma once

#include <filesystem>

namespace gv::core {

// Installs process-wide handlers for unhandled Win32 SEH, unhandled C++
// exceptions, std::terminate, and pure-virtual / invalid-parameter CRT
// failures. On any of these, writes a minidump to `dump_dir` and a textual
// stack frame to the log before letting the process exit.
//
// Call once, very early in main() (after Logger::init).
class CrashHandler
{
public:
   static void install (const std::filesystem::path& dump_dir);
};

} // namespace gv::core

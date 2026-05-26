#include <gv/core/crash_handler.h>
#include <gv/core/logger.h>

#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <stdlib.h>     // _set_invalid_parameter_handler, _set_purecall_handler
#include <string>

#pragma comment (lib, "Dbghelp.lib")

namespace gv::core {

namespace {

   std::filesystem::path g_dump_dir;
   std::atomic<bool>     g_handling { false };

   std::wstring make_dump_path ()
   {
      using namespace std::chrono;
      const auto t = system_clock::to_time_t (system_clock::now ());
      tm bt {};
      ::localtime_s (&bt, &t);

      wchar_t stamp [32];
      std::wcsftime (stamp, 32, L"crash-%Y%m%d-%H%M%S.dmp", &bt);

      auto full = g_dump_dir / stamp;
      return full.wstring ();
   }

   void write_minidump (EXCEPTION_POINTERS* ep)
   {
      const auto path = make_dump_path ();

      HANDLE file = ::CreateFileW (path.c_str (),
         GENERIC_WRITE, 0, nullptr,
         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

      if (file == INVALID_HANDLE_VALUE) {
         Logger::error ("crash_handler: cannot open dump file");
         return;
      }

      MINIDUMP_EXCEPTION_INFORMATION mei {};
      mei.ThreadId          = ::GetCurrentThreadId ();
      mei.ExceptionPointers = ep;
      mei.ClientPointers    = FALSE;

      const BOOL ok = ::MiniDumpWriteDump (
         ::GetCurrentProcess (),
         ::GetCurrentProcessId (),
         file,
         static_cast<MINIDUMP_TYPE> (
            MiniDumpWithThreadInfo |
            MiniDumpWithIndirectlyReferencedMemory |
            MiniDumpWithUnloadedModules),
         ep ? &mei : nullptr,
         nullptr,
         nullptr);

      ::CloseHandle (file);

      if (ok) {
         Logger::error ("crash_handler: wrote minidump to {}",
            std::filesystem::path { path }.string ());
      } else {
         Logger::error ("crash_handler: MiniDumpWriteDump failed (err={})", ::GetLastError ());
      }
   }

   LONG WINAPI seh_filter (EXCEPTION_POINTERS* ep)
   {
      if (g_handling.exchange (true)) {
         return EXCEPTION_EXECUTE_HANDLER;  // re-entry guard
      }

      const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
      Logger::error ("crash_handler: SEH exception 0x{:08X}", code);

      write_minidump (ep);
      Logger::shutdown ();
      return EXCEPTION_EXECUTE_HANDLER;
   }

   void terminate_handler ()
   {
      if (g_handling.exchange (true)) {
         std::abort ();
      }

      Logger::error ("crash_handler: std::terminate invoked");

      // No EXCEPTION_POINTERS in this path — pass null and minidump will still
      // capture all thread state.
      write_minidump (nullptr);
      Logger::shutdown ();
      std::abort ();
   }

   void invalid_parameter_handler (
      const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
   {
      Logger::error ("crash_handler: CRT invalid_parameter");
      write_minidump (nullptr);
      Logger::shutdown ();
      std::abort ();
   }

   void purecall_handler ()
   {
      Logger::error ("crash_handler: pure virtual call");
      write_minidump (nullptr);
      Logger::shutdown ();
      std::abort ();
   }

} // namespace

void CrashHandler::install (const std::filesystem::path& dump_dir)
{
   g_dump_dir = dump_dir;
   std::error_code ec;
   std::filesystem::create_directories (g_dump_dir, ec);

   ::SetUnhandledExceptionFilter (&seh_filter);

   std::set_terminate          (&terminate_handler);
   _set_invalid_parameter_handler (&invalid_parameter_handler);
   _set_purecall_handler         (&purecall_handler);

   Logger::info ("crash_handler: installed (dumps → {})", g_dump_dir.string ());
}

} // namespace gv::core

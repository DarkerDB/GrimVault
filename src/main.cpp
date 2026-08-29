#include <gv/api/darkerdb_client.h>
#include <gv/app/controller.h>
#include <gv/app/runtime.h>
#include <gv/app/settings_bridge.h>
#include <gv/app/settings_sync.h>
#include <gv/auth/oauth_client.h>
#include <gv/auth/session.h>
#include <gv/capture/capture_service.h>
#include <gv/cli/cli.h>
#include <gv/collection/collector.h>
#include <gv/collection/log_artifact.h>
#include <gv/core/crash_handler.h>
#include <gv/core/diagnostics.h>
#include <gv/core/env.h>
#include <gv/core/env_resolver.h>
#include <gv/core/hotkey_manager.h>
#include <gv/core/http.h>
#include <gv/core/ini_migrator.h>
#include <gv/core/logger.h>
#include <gv/core/single_instance.h>
#include <gv/core/version.h>
#include <gv/core/window_tracker.h>
#include <gv/db/database.h>
#include <gv/db/repos/user_hotkeys_repo.h>
#include <gv/db/repos/user_settings_repo.h>
#include <gv/ocr/capture_policy.h>
#include <gv/ocr/language_registry.h>
#include <gv/ocr/pipeline.h>
#include <gv/ui/debug_overlay.h>
#include <gv/ui/overlay_window.h>
#include <gv/ui/status_badge.h>
#include <gv/ui/tray_icon.h>
#include <gv/update/update_service.h>
#include <gv/vision/tooltip_detector.h>

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDir>
#include <QFontDatabase>
#include <QPointer>
#include <QStandardPaths>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>

#ifdef _WIN32
#include <ShellScalingApi.h>
#include <Windows.h>
#include <crtdbg.h>
#include <fcntl.h>
#include <io.h>
#endif

#include <algorithm>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

// Fatal-path visibility. Debug-CRT asserts and aborts pop interactive
// dialogs by default — they bypass WER and the SEH crash handler, so an
// uncaught exception reads as silent process death with the last log
// lines still buffered. Route CRT reports to stderr and log the active
// exception from std::terminate, flushing before going down.
void install_fatal_handlers ()
{
#if defined(_WIN32) && defined(_DEBUG)
   _CrtSetReportMode (_CRT_ASSERT, _CRTDBG_MODE_FILE);
   _CrtSetReportFile (_CRT_ASSERT, _CRTDBG_FILE_STDERR);
   _CrtSetReportMode (_CRT_ERROR, _CRTDBG_MODE_FILE);
   _CrtSetReportFile (_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif

   std::set_terminate ([] {
      std::string what = "std::terminate (no active exception)";

      if (auto ex = std::current_exception ()) {
         try {
            std::rethrow_exception (ex);
         } catch (const std::exception& e) {
            what = std::string { "uncaught exception: " } + e.what ();
         } catch (...) {
            what = "uncaught non-std exception";
         }
      }

      gv::core::Logger::error ("FATAL: {}", what);
      gv::core::Logger::shutdown ();
      std::abort ();
   });
}

// ---- GUI run loop ----

void apply_debug_option (gv::app::GuiOptions& opts, std::string_view arg)
{
   opts.debug = true;
   if (arg == "--debug") return;

   constexpr std::string_view prefix = "--debug=";
   if (!arg.starts_with (prefix)) return;

   std::string_view selectors = arg.substr (prefix.size ());
   while (!selectors.empty ()) {
      const auto comma = selectors.find (',');
      const auto item = selectors.substr (0, comma);

      if (item == "highlight:objects") opts.highlight_objects = true;
      else if (item == "highlight:game") opts.highlight_game = true;
      else if (!item.empty ()) {
         std::fprintf (stderr, "unknown --debug selector: %.*s\n", static_cast<int> (item.size ()),
                       item.data ());
      }

      if (comma == std::string_view::npos) break;
      selectors.remove_prefix (comma + 1);
   }
}

double consume_fcr (std::vector<std::string>& args)
{
   double fcr = 0.0;

   for (std::size_t i = 0; i < args.size (); ++i) {
      std::string value;

      if (args[i] == "--fcr" && i + 1 < args.size ()) {
         value = args[i + 1];
         args.erase (args.begin () + static_cast<std::ptrdiff_t> (i),
                     args.begin () + static_cast<std::ptrdiff_t> (i) + 2);
      } else if (args[i].rfind ("--fcr=", 0) == 0) {
         value = args[i].substr (6);
         args.erase (args.begin () + static_cast<std::ptrdiff_t> (i));
      } else {
         continue;
      }

      try {
         fcr = std::stod (value);
      } catch (...) {
         fcr = 0.0;
      }
      break;
   }

   if (fcr <= 0.0) return 0.0;
   return std::clamp (fcr, gv::ocr::minimum_capture_fps, 60.0);
}

#ifdef _WIN32
// `grimvault --detached`: relaunch without the flag, detached from this
// console, and exit. The default (foreground) run keeps the invoking
// terminal and streams logs to it; this is the opt-out.
int relaunch_detached (const std::vector<std::string>& args)
{
   wchar_t exe[MAX_PATH] {};
   ::GetModuleFileNameW (nullptr, exe, MAX_PATH);

   std::wstring cmd = L"\"";
   cmd += exe;
   cmd += L"\"";

   for (const auto& a : args) {
      if (a == "--detached") continue;
      cmd += L" \"";
      cmd += std::filesystem::path { a }.wstring ();
      cmd += L"\"";
   }

   STARTUPINFOW si { .cb = sizeof (STARTUPINFOW) };
   PROCESS_INFORMATION pi {};

   const BOOL ok =
      ::CreateProcessW (nullptr, cmd.data (), nullptr, nullptr, FALSE,
                        DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);

   if (!ok) {
      std::fprintf (stderr, "failed to relaunch detached (error %lu)\n", ::GetLastError ());
      return 1;
   }

   ::CloseHandle (pi.hThread);
   ::CloseHandle (pi.hProcess);
   std::printf ("grimvault started in the background (pid %lu)\n", pi.dwProcessId);
   return 0;
}
#endif

}  // namespace

int main (int argc, char** argv)
{
   install_fatal_handlers ();
   gv::app::enable_dpi ();

   // Force the static-lib .qrc initializers to link in. Without this, MSVC's
   // linker strips the resources' anonymous-namespace init from the static
   // libs because nothing else references them.
   Q_INIT_RESOURCE (qml);
   Q_INIT_RESOURCE (auth);

   // Dual-mode dispatch:
   //    - argc == 1                → GUI mode (tray + overlay)
   //    - argv contains only flags
   //      consumed by run_gui      → GUI mode
   //    - otherwise                → CLI mode (subcommand + flags)
   //
   // Recognized GUI-only flags: --hidden (from autostart entry),
   // --no-auto-login (skip the on-launch OAuth prompt), --debug (verbose
   // logs + OCR stage dumps), --debug=highlight:objects,highlight:game
   // (explicit diagnostic borders), --detached (relaunch in the
   // background and return immediately), --detect-only (stop the pipeline
   // after detection; no OCR / lookup / augment), --fcr <n> (active frame
   // capture rate, 1-60 fps).
   std::vector<std::string> cli_args;
   for (int i = 1; i < argc; ++i) cli_args.emplace_back (argv[i]);

   // Resolve the active env up front so both GUI and CLI dispatch see the
   // same value. --env / --env=<name> is consumed from cli_args here.
   gv::core::set_active_env (gv::core::resolve_active_env (cli_args));

   const double fcr = consume_fcr (cli_args);

   const auto is_gui_flag = [] (const std::string& a) {
      return a == "--hidden" || a == "--no-auto-login" || a == "--debug" ||
             a.rfind ("--debug=", 0) == 0 || a == "--detached" || a == "--detect-only" ||
             a == "--ocr-only";
   };

   const bool all_gui_flags =
      !cli_args.empty () && std::all_of (cli_args.begin (), cli_args.end (), is_gui_flag);

   if (cli_args.empty () || all_gui_flags) {
#ifdef _WIN32
      if (std::find (cli_args.begin (), cli_args.end (), "--detached") != cli_args.end ()) {
         gv::app::attach_console ();
         return relaunch_detached (cli_args);
      }
#endif

      gv::app::GuiOptions opts;
      opts.fcr = fcr;
      for (const auto& a : cli_args) {
         if (a == "--no-auto-login") opts.no_auto_login = true;
         if (a == "--debug" || a.rfind ("--debug=", 0) == 0) apply_debug_option (opts, a);
         if (a == "--detect-only") opts.detect_only = true;
         if (a == "--ocr-only") opts.ocr_only = true;
      }
      return gv::app::run_gui (argc, argv, opts);
   }
   return gv::app::run_cli (argc, argv, cli_args);
}

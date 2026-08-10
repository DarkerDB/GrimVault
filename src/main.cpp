#include <gv/api/darkerdb_client.h>
#include <gv/app/controller.h>
#include <gv/app/settings_bridge.h>
#include <gv/app/settings_sync.h>
#include <gv/auth/oauth_client.h>
#include <gv/auth/session.h>
#include <gv/capture/capture_service.h>
#include <gv/cli/cli.h>
#include <gv/core/crash_handler.h>
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

#ifdef _WIN32
   #include <Windows.h>
   #include <ShellScalingApi.h>
   #include <crtdbg.h>
   #include <io.h>
   #include <fcntl.h>
#endif

#include <exception>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

   constexpr const char* k_single_instance_mutex = "GrimVault.SingleInstance.v1";
   constexpr const char* k_surface_message       = "GrimVault.Surface.v1";

   bool env_enabled (const char* name)
   {
      const auto value = qEnvironmentVariable (name);
      return !value.isEmpty () && value != "0";
   }

   // ---- console attach for CLI on Windows ----
   //
   // The binary is linked as a WINDOWS subsystem app so GUI launch never
   // shows a console flash. Any invocation from an existing terminal (CLI
   // subcommands, GUI runs watching stdout logs) needs explicit
   // AttachConsole + stdout/stderr reopen so output makes it back to the
   // parent terminal. No-op when there is no parent console (double-click,
   // autostart), so GUI launches stay flash-free.
   void attach_parent_console ()
   {
#ifdef _WIN32
      // Snapshot redirection state BEFORE AttachConsole — attaching installs
      // fresh console handles, which would make every stream look "already
      // valid" even though the CRT streams of a WINDOWS-subsystem app start
      // dead. Only streams the parent didn't redirect (pipe, file, WSL
      // interop) get reopened onto the console; redirected ones keep their
      // inherited handle or `grimvault status > out.txt` would write to the
      // console instead of the file.
      auto redirected = [] (DWORD std_handle) {
         HANDLE h = ::GetStdHandle (std_handle);
         return h != nullptr && h != INVALID_HANDLE_VALUE && ::GetFileType (h) != FILE_TYPE_UNKNOWN;
      };

      const bool out_redirected = redirected (STD_OUTPUT_HANDLE);
      const bool err_redirected = redirected (STD_ERROR_HANDLE);
      const bool in_redirected  = redirected (STD_INPUT_HANDLE);

      if (::AttachConsole (ATTACH_PARENT_PROCESS)) {
         FILE* dummy = nullptr;

         if (!out_redirected) freopen_s (&dummy, "CONOUT$", "w", stdout);
         if (!err_redirected) freopen_s (&dummy, "CONOUT$", "w", stderr);
         if (!in_redirected)  freopen_s (&dummy, "CONIN$",  "r", stdin);
         std::ios::sync_with_stdio ();
      }
#endif
   }

   // Fatal-path visibility. Debug-CRT asserts and aborts pop interactive
   // dialogs by default — they bypass WER and the SEH crash handler, so an
   // uncaught exception reads as silent process death with the last log
   // lines still buffered. Route CRT reports to stderr and log the active
   // exception from std::terminate, flushing before going down.
   void install_fatal_handlers ()
   {
#if defined (_WIN32) && defined (_DEBUG)
      _CrtSetReportMode (_CRT_ASSERT, _CRTDBG_MODE_FILE);
      _CrtSetReportFile (_CRT_ASSERT, _CRTDBG_FILE_STDERR);
      _CrtSetReportMode (_CRT_ERROR,  _CRTDBG_MODE_FILE);
      _CrtSetReportFile (_CRT_ERROR,  _CRTDBG_FILE_STDERR);
#endif

      std::set_terminate ([] {
         std::string what = "std::terminate (no active exception)";

         if (auto ex = std::current_exception ()) {
            try { std::rethrow_exception (ex); }
            catch (const std::exception& e) {
               what = std::string { "uncaught exception: " } + e.what ();
            }
            catch (...) {
               what = "uncaught non-std exception";
            }
         }

         gv::core::Logger::error ("FATAL: {}", what);
         gv::core::Logger::shutdown ();
         std::abort ();
      });
   }

#ifdef _WIN32
   // Ctrl+C / Ctrl+Break from an attached terminal: route through the Qt
   // event loop so shutdown runs cleanly (tray icon removed, pipeline
   // stopped) instead of the default handler's abrupt ExitProcess, which
   // leaves a ghost tray icon behind.
   BOOL WINAPI console_ctrl_handler (DWORD type)
   {
      switch (type) {
         case CTRL_C_EVENT:
         case CTRL_BREAK_EVENT:
            if (auto* app = QCoreApplication::instance ()) {
               QMetaObject::invokeMethod (app,
                  &QCoreApplication::quit, Qt::QueuedConnection);
               return TRUE;
            }
            return FALSE;
         default:
            return FALSE;
      }
   }
#endif

   std::filesystem::path app_data_dir ()
   {
      auto qpath = QStandardPaths::writableLocation (QStandardPaths::GenericDataLocation)
         + QStringLiteral ("/GrimVault");
      const auto env = gv::core::active_env ().name;
      if (env != "prod") {
         qpath += QStringLiteral ("/")
            + QString::fromUtf8 (env.data (), static_cast<qsizetype> (env.size ()));
      }
      QDir ().mkpath (qpath);
      return std::filesystem::path { qpath.toStdWString () };
   }

   gv::core::Result<bool> scope_settings (
      gv::db::UserSettingsRepo& repo,
      const std::optional<std::string>& principal)
   {
      constexpr std::string_view marker = "auth:subject";
      auto stored = repo.get (marker);
      if (!stored.has_value ()) return gv::core::fail (stored.error ());

      const bool has_marker = stored.has_value () && stored->has_value ();
      const std::string next = principal.value_or ("");
      const std::string current = has_marker ? **stored : "";

      // First V2 launch adopts existing V1 settings for the current account.
      if (!has_marker && !next.empty ()) {
         auto saved = repo.set (std::string { marker }, next);
         if (!saved.has_value ()) return gv::core::fail (saved.error ());
         return false;
      }
      if (has_marker && current == next) return false;

      auto all = repo.all ();
      if (!all.has_value ()) return gv::core::fail (all.error ());

      constexpr std::string_view prefixes [] = {
         "behavior:", "hotkeys:", "overlay:", "pricing:", "tooltip:"
      };
      for (const auto& [key, value] : *all) {
         (void) value;
         if (key == "overlay:renderer") continue;
         const bool managed = std::any_of (
            std::begin (prefixes), std::end (prefixes),
            [&key] (std::string_view prefix) { return key.starts_with (prefix); });
         if (!managed) continue;

         auto erased = repo.erase (key);
         if (!erased.has_value ()) return gv::core::fail (erased.error ());
      }

      auto marked = next.empty ()
         ? repo.erase (std::string { marker })
         : repo.set (std::string { marker }, next);
      if (!marked.has_value ()) return gv::core::fail (marked.error ());
      return true;
   }

   std::filesystem::path resolve_install_dir ()
   {
      const auto resources = qEnvironmentVariable ("GRIMVAULT_DEV_RESOURCES");
      if (!resources.isEmpty ()) {
         return std::filesystem::path { resources.toStdWString () };
      }
      return std::filesystem::path { QCoreApplication::applicationDirPath ().toStdWString () };
   }

   void enable_per_monitor_dpi ()
   {
#ifdef _WIN32
      using SetCtxFn = BOOL (WINAPI*) (DPI_AWARENESS_CONTEXT);

      if (auto* mod = ::GetModuleHandleW (L"user32.dll")) {
         if (auto fn = reinterpret_cast<SetCtxFn> (::GetProcAddress (mod, "SetProcessDpiAwarenessContext"))) {
            fn (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            return;
         }
      }
      ::SetProcessDpiAwareness (PROCESS_PER_MONITOR_DPI_AWARE);
#endif
   }

   void register_app_fonts ()
   {
      for (const auto* path : {
         ":/assets/fonts/SaintKDG_Light.ttf",
         ":/assets/fonts/SaintKDG_Medium.ttf",
         ":/assets/fonts/Pelagiad.ttf",
      }) {
         const int id = QFontDatabase::addApplicationFont (QString::fromLatin1 (path));
         gv::core::Logger::info ("fonts: {} -> [{}]", path,
            id >= 0
               ? QFontDatabase::applicationFontFamilies (id).join (", ").toStdString ()
               : std::string { "LOAD FAILED" });
      }
   }

   // Route Qt's qDebug/qWarning/qCritical/qFatal into our logger so QML
   // parse errors, plugin-load failures, and binding warnings land in the
   // same place as everything else.
   void qt_message_handler (QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
   {
      const std::string txt = msg.toStdString ();
      const std::string where = ctx.category ? std::string { "qt." } + ctx.category : std::string { "qt" };

      switch (type) {
         case QtDebugMsg:    gv::core::Logger::debug ("{}: {}", where, txt); break;
         case QtInfoMsg:     gv::core::Logger::info  ("{}: {}", where, txt); break;
         case QtWarningMsg:  gv::core::Logger::warn  ("{}: {}", where, txt); break;
         case QtCriticalMsg: gv::core::Logger::error ("{}: {}", where, txt); break;
         case QtFatalMsg:    gv::core::Logger::error ("{}: {} (fatal)", where, txt); break;
      }
   }

   // ---- GUI run loop ----

   struct GuiOptions {
      bool   no_auto_login = false;
      bool   debug         = false;
      bool   highlight_objects = false;
      bool   highlight_game    = false;
      bool   detect_only   = false;
      double fcr           = 0.0;   // frames/s while active; 0 = default
   };

   void apply_debug_option (GuiOptions& opts, std::string_view arg)
   {
      opts.debug = true;
      if (arg == "--debug") return;

      constexpr std::string_view prefix = "--debug=";
      if (!arg.starts_with (prefix)) return;

      std::string_view selectors = arg.substr (prefix.size ());
      while (!selectors.empty ()) {
         const auto comma = selectors.find (',');
         const auto item  = selectors.substr (0, comma);

         if (item == "highlight:objects") opts.highlight_objects = true;
         else if (item == "highlight:game") opts.highlight_game = true;
         else if (!item.empty ()) {
            std::fprintf (stderr, "unknown --debug selector: %.*s\n",
               static_cast<int> (item.size ()), item.data ());
         }

         if (comma == std::string_view::npos) break;
         selectors.remove_prefix (comma + 1);
      }
   }

   // Consume `--fcr <n>` / `--fcr=<n>` from args; returns 0 when absent.
   // Clamped to [1, 60] — below 1 the overlay feels dead, above 60 the
   // detector can't keep up anyway and the capture thread just spins.
   double consume_fcr (std::vector<std::string>& args)
   {
      double fcr = 0.0;

      for (std::size_t i = 0; i < args.size (); ++i) {
         std::string value;

         if (args [i] == "--fcr" && i + 1 < args.size ()) {
            value = args [i + 1];
            args.erase (args.begin () + static_cast<std::ptrdiff_t> (i),
                        args.begin () + static_cast<std::ptrdiff_t> (i) + 2);
         } else if (args [i].rfind ("--fcr=", 0) == 0) {
            value = args [i].substr (6);
            args.erase (args.begin () + static_cast<std::ptrdiff_t> (i));
         } else {
            continue;
         }

         try { fcr = std::stod (value); } catch (...) { fcr = 0.0; }
         break;
      }

      if (fcr <= 0.0) return 0.0;
      return std::clamp (fcr, 1.0, 60.0);
   }

#ifdef _WIN32
   // `grimvault --detached`: relaunch without the flag, detached from this
   // console, and exit. The default (foreground) run keeps the invoking
   // terminal and streams logs to it; this is the opt-out.
   int relaunch_detached (const std::vector<std::string>& args)
   {
      wchar_t exe [MAX_PATH] {};
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

      STARTUPINFOW        si { .cb = sizeof (STARTUPINFOW) };
      PROCESS_INFORMATION pi {};

      const BOOL ok = ::CreateProcessW (
         nullptr, cmd.data (), nullptr, nullptr, FALSE,
         DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP,
         nullptr, nullptr, &si, &pi);

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

   int run_gui (int argc, char** argv, GuiOptions opts)
   {
      attach_parent_console ();

      auto single_instance = gv::core::SingleInstanceGuard::acquire (k_single_instance_mutex);
      if (!single_instance) {
         gv::core::SingleInstanceGuard::notify_existing (k_surface_message);
         return 0;
      }

      gv::core::http::Global http;
      if (!http) return 1;

      // Qt Quick's threaded render loop can deadlock main in polishAndSync
      // when small always-on-top windows (status badge, debug overlay) get
      // exposed/re-stacked in bursts as the game window appears. The QML
      // surfaces here are tiny; the single-threaded loop costs nothing and
      // removes that whole hazard class.
      qputenv ("QSG_RENDER_LOOP", "basic");

      QApplication app (argc, argv);
      app.setApplicationName    (QStringLiteral ("GrimVault"));
      app.setOrganizationName   (QStringLiteral ("DDB"));
      app.setOrganizationDomain (QStringLiteral ("darkerdb.com"));
      app.setApplicationVersion (QString::fromLatin1 (gv::core::version::string));
      app.setQuitOnLastWindowClosed (false);
      app.setWindowIcon (QIcon (QStringLiteral (":/assets/images/Icon-324x356.png")));

#ifdef _WIN32
      ::SetConsoleCtrlHandler (&console_ctrl_handler, TRUE);
#endif

      const auto data_dir = app_data_dir ();

      gv::core::Logger::init (data_dir / "logs", /*verbose=*/ opts.debug);
      qInstallMessageHandler (&qt_message_handler);

      if (opts.debug) {
#ifdef _WIN32
         // Pipeline reads this at first scan; dumps land in %TEMP%\grimvault-ocr.
         ::_putenv_s ("GRIMVAULT_OCR_DEBUG", "1");
         ::_putenv_s ("GRIMVAULT_ANCHOR_DIAGNOSTICS",
            (data_dir / "logs" / "anchoring").string ().c_str ());
#endif
         gv::core::Logger::info ("debug mode: verbose logs + OCR stage dumps enabled");
         gv::core::Logger::info ("debug highlights: objects={}, game={}",
            opts.highlight_objects, opts.highlight_game);
      }

      const auto& active_env = gv::core::active_env ();
      gv::core::Logger::info ("GrimVault {} (env={}, api={}, auth={})",
         gv::core::version::string,
         std::string { active_env.name },
         std::string { active_env.api_base_url },
         std::string { active_env.auth_base_url });
      if (active_env.name == "dev" && env_enabled ("GRIMVAULT_INSECURE_DEV_TLS")) {
         gv::core::Logger::warn (
            "TLS verification explicitly disabled for env=dev");
      }

      gv::core::CrashHandler::install (data_dir / "logs");

      auto db = gv::db::Database::open (data_dir / "grimvault.db");
      if (!db.has_value ()) {
         gv::core::Logger::error ("Failed to open database: {}", db.error ().message);
         return 1;
      }

      gv::db::UserSettingsRepo settings_repo { **db };
      gv::db::UserHotkeysRepo  hotkeys_repo  { **db };

      if (auto m = gv::core::IniMigrator::run (data_dir / "settings.ini", settings_repo, hotkeys_repo);
          !m.has_value ()) {
         gv::core::Logger::error ("Settings init failed: {}", m.error ().message);
         return 1;
      }

      const auto install_dir = resolve_install_dir ();
      gv::core::Logger::info ("resources: {}", install_dir.string ());

      register_app_fonts ();

      // ---- Auth wiring ----
      gv::auth::OauthClient::Config oauth_cfg;
      oauth_cfg.client_id     = std::string { active_env.client_id };
      oauth_cfg.api_base_url  = std::string { active_env.api_base_url };
      oauth_cfg.auth_base_url = std::string { active_env.auth_base_url };
      oauth_cfg.spa_base_url  = std::string { active_env.spa_base_url };
      auto oauth = std::make_shared<gv::auth::OauthClient> (std::move (oauth_cfg));

      gv::auth::Session session { oauth };
      if (auto scoped = scope_settings (settings_repo, session.principal ());
          !scoped.has_value ()) {
         gv::core::Logger::error ("Settings account scope failed: {}", scoped.error ().message);
         return 1;
      }

      // ---- Overlay + capture pipeline ----
      //
      // WebView2's virtual-host mapping wants a local drive: in dev the
      // resources root is a network mapping (W: -> WSL), which Chromium
      // treats differently. The post-build step stages web/ next to the
      // exe, so prefer that copy when present.
      auto web_dir = install_dir / "web";
      if (const auto staged = std::filesystem::path {
             QCoreApplication::applicationDirPath ().toStdWString () } / "web";
          std::filesystem::exists (staged / "augment.html")) {
         web_dir = staged;
      }

      gv::ui::OverlayWindow::Config overlay_cfg {
         .web_dir       = std::move (web_dir),
         .user_data_dir = data_dir / "webview2",
      };
      if (auto v = settings_repo.get ("overlay:renderer"); v.has_value () && v->has_value ()) {
         overlay_cfg.renderer = **v;
      }
      gv::ui::OverlayWindow overlay { std::move (overlay_cfg) };
      gv::ui::DebugOverlay  debug_overlay;

      gv::api::DDBClient::Config api_cfg;
      api_cfg.base_url  = std::string { active_env.api_base_url };
      api_cfg.client_id = std::string { active_env.client_id };
      gv::api::DDBClient api_client { api_cfg, &session, db->get () };

      gv::vision::TooltipDetector detector;
      if (auto r = detector.initialize (install_dir / "models" / "tooltip.onnx"); !r.has_value ()) {
         gv::core::Logger::warn ("vision: tooltip detector init failed: {}", r.error ().message);
      }

      gv::ocr::LanguageRegistry langs { install_dir / "models", /*max_resident=*/ 2 };

      std::unique_ptr<gv::capture::CaptureService> capture;
      if (auto cs = gv::capture::CaptureService::create (); cs.has_value ()) {
         capture = std::move (*cs);
         gv::core::Logger::info ("capture: probe selected {}", std::string { capture->current ().name () });
      } else {
         gv::core::Logger::warn ("capture: not available at startup ({})", cs.error ().message);
      }

      // Seed the stored capture-backend policy before the pipeline spins up
      // so a forced backend applies from the first frame instead of after
      // the first settings poll.
      if (capture) {
         if (auto v = settings_repo.get ("behavior:capture_mode");
             v.has_value () && v->has_value ()) {
            if (const auto mode = gv::capture::parse_capture_mode (**v);
                mode.has_value ()) {
               if (auto r = capture->set_mode (*mode); !r.has_value ()) {
                  gv::core::Logger::warn ("capture: stored mode {} rejected: {}",
                     **v, r.error ().message);
               }
            }
         }
      }

      std::unique_ptr<gv::ocr::Pipeline> pipeline;
      if (capture) {
         gv::ocr::Pipeline::Config pipe_cfg;
         if (opts.fcr > 0.0) {
            pipe_cfg.active_fps = opts.fcr;
            pipe_cfg.capture_fps = opts.fcr;
            gv::core::Logger::info ("pipeline: frame capture rate {} fps (--fcr)", opts.fcr);
         }
         if (opts.debug) {
            pipe_cfg.sample_inbox = data_dir / "ocr-samples" / "inbox";
            gv::core::Logger::info ("OCR sample inbox: {}",
               pipe_cfg.sample_inbox.string ());
         }

         pipeline = std::make_unique<gv::ocr::Pipeline> (
            *capture, detector, langs, pipe_cfg);

         if (opts.detect_only) {
            pipeline->set_detect_only (true);
            gv::core::Logger::info ("pipeline: detect-only mode (OCR / lookup / augment disabled)");
         }
      }

      std::unique_ptr<gv::core::HotkeyManager> hotkeys;
      if (auto hk = gv::core::HotkeyManager::create (); hk.has_value ()) {
         hotkeys = std::move (*hk);
      } else {
         gv::core::Logger::error ("hotkeys: manager init failed: {}", hk.error ().message);
      }

      gv::update::UpdateService update_service;

      gv::app::Controller::Dependencies deps {
         .db            = db->get (),
         .hotkeys_repo  = &hotkeys_repo,
         .settings_repo = &settings_repo,
         .api           = &api_client,
         .pipeline      = pipeline.get (),
         .hotkeys       = hotkeys.get (),
         .overlay       = &overlay,
         .debug         = &debug_overlay,
         .highlight_game = opts.highlight_game,
         .highlight_objects = opts.highlight_objects,
      };
      gv::app::Controller controller { deps };
      controller.set_authenticated (
         session.signed_in (), session.principal ().value_or (""));

      std::unique_ptr<gv::core::WindowTracker> tracker;
      if (auto t = gv::core::WindowTracker::create (
            {}, [&controller] (const gv::core::WindowEvent& ev) {
               controller.on_window_event (ev);
            });
          t.has_value ()) {
         tracker = std::move (*t);
      } else {
         gv::core::Logger::warn ("window_tracker init failed: {}", t.error ().message);
      }

      if (pipeline) {
         // Badge pulse at detection time (vision thread) — OCR + lookup can
         // take seconds in debug builds; without this the badge sits idle
         // while visibly "nothing happens".
         pipeline->on_activity ([&controller] {
            QMetaObject::invokeMethod (&controller, [&controller] {
               emit controller.scanActivity ();
            }, Qt::QueuedConnection);
         });

         auto start_r = pipeline->start ([&controller] (const gv::ocr::RecognizedTooltip& rt) {
            controller.on_tooltip (rt);
         });
         if (!start_r.has_value ()) {
            gv::core::Logger::error ("pipeline: start failed: {}", start_r.error ().message);
         }
      }

      controller.set_browse_base (std::string { active_env.spa_base_url });

      const int bound = controller.bind_hotkeys_from_repo ();
      gv::core::Logger::info ("hotkeys: {} bindings active", bound);

      // Env flag wins over the dashboard toggle so dev/CI can suppress
      // updates unconditionally. Resolved before the bridge so its first
      // reload () already knows updates are locked off.
      const bool disabled_by_env = env_enabled ("GRIMVAULT_DISABLE_UPDATES");

      // ---- Settings application ----
      // The half of settings that makes them do something: folds the stored
      // keys into live overlay layout, card options, overlay mode, hotkey
      // bindings and the launch-on-startup entry. Seeded from the repo here
      // so a signed-in relaunch is already correct before the first poll.
      gv::app::SettingsBridge settings_bridge {{
         .repo               = &settings_repo,
         .overlay            = &overlay,
         .controller         = &controller,
         .exe_path           = app.applicationFilePath ().toStdString (),
         .updates_locked_off = disabled_by_env,
         .capture_fps_locked = opts.fcr > 0.0,
      }};
      settings_bridge.reload ();

      // ---- Tray icon ----
      if (!QSystemTrayIcon::isSystemTrayAvailable ()) {
         gv::core::Logger::error ("system tray not available; exiting");
         return 1;
      }

      gv::ui::TrayIcon tray;
      tray.set_connection_state (session.signed_in ()
         ? gv::ui::ConnectionState::Syncing
         : gv::ui::ConnectionState::SignedOut);

      // In-game corner badge: pinned bottom-right of the game window, shows
      // sign-in dot + scan mode, pulses on pipeline activity.
      gv::ui::StatusBadge badge;
      badge.set_signed_in (session.signed_in ());
      QObject::connect (&controller, &gv::app::Controller::gameWindowChanged,
         &badge, &gv::ui::StatusBadge::set_game);
      QObject::connect (&controller, &gv::app::Controller::scanActivity,
         &badge, &gv::ui::StatusBadge::pulse);
      QObject::connect (&controller, &gv::app::Controller::modeChanged,
         &badge, [&badge] (gv::app::Mode m) {
            badge.set_auto (m == gv::app::Mode::Auto);
         });

      // ---- Settings sync ----
      // Dashboard-controlled settings polled from /v2/grimvault/settings and
      // mirrored into UserSettingsRepo. Starts on sign-in, stops on sign-out.
      // Dev polls hard: the API is a container on the same box, and the
      // whole point of the dev loop is changing a setting on the dashboard
      // and watching the card change.
      gv::app::SettingsSync::Config sync_cfg;
      if (active_env.name == "dev") {
         sync_cfg.interval      = std::chrono::seconds { 5 };
         sync_cfg.backoff_floor = std::chrono::seconds { 5 };
      }

      gv::app::SettingsSync settings_sync {
         &api_client, &session, &settings_repo, sync_cfg };
      QObject::connect (&settings_sync, &gv::app::SettingsSync::settings_changed,
         &settings_bridge, &gv::app::SettingsBridge::apply);

      // Canonical signed-in state as the GUI last reconciled it. Updated by
      // the in-process sign-in/out handlers below and by the external-change
      // watcher, so neither double-reacts to a flip the other already handled.
      auto signed_state = std::make_shared<bool> (session.signed_in ());
      auto signed_subject = std::make_shared<std::string> (
         session.principal ().value_or (""));
      auto settings_ready = std::make_shared<bool> (false);
      auto settings_failure_notified = std::make_shared<bool> (false);
      std::unordered_set<QThread*> oauth_workers;
      bool sign_in_active = false;

      // ---- Sign-in: run the OAuth flow off the Qt main thread so the UI
      // stays responsive while the loopback server blocks on the callback.
      auto do_sign_in = [&] {
         if (sign_in_active) {
            gv::core::log::app.debug ("sign-in already in progress");
            return;
         }
         sign_in_active = true;

         tray.showMessage (QStringLiteral ("GrimVault"),
            QStringLiteral ("Opening your browser to sign in…"),
            QSystemTrayIcon::Information, 4000);

         auto* worker = QThread::create ([&, oauth] {
            auto resp = oauth->authorize ();
            QMetaObject::invokeMethod (&app, [&, resp = std::move (resp)] () mutable {
               if (!resp.has_value ()) {
                  tray.showMessage (QStringLiteral ("Sign-in failed"),
                     QString::fromStdString (resp.error ().message),
                     QSystemTrayIcon::Critical, 8000);
                  return;
               }
               auto inst = session.install (resp->tokens);
               if (!inst.has_value ()) {
                  tray.showMessage (QStringLiteral ("Sign-in failed"),
                     QString::fromStdString (inst.error ().message),
                     QSystemTrayIcon::Critical, 8000);
                  return;
               }
               const auto principal = session.principal ();
               auto scoped = scope_settings (settings_repo, principal);
               if (!scoped.has_value ()) {
                  (void) session.sign_out (/*local_only=*/ true);
                  tray.showMessage (QStringLiteral ("Sign-in failed"),
                     QStringLiteral ("Could not isolate settings for this account."),
                     QSystemTrayIcon::Critical, 8000);
                  gv::core::log::app.error (
                     "settings account scope failed: {}", scoped.error ().message);
                  return;
               }
               settings_bridge.reload ();
               *signed_state = true;
               *signed_subject = principal.value_or ("");
               *settings_ready = false;
               *settings_failure_notified = false;
               controller.set_authenticated (true, principal.value_or (""));
               tray.set_connection_state (gv::ui::ConnectionState::Syncing);
               badge.set_signed_in (true);
               tray.showMessage (QStringLiteral ("Signed in"),
                  QStringLiteral ("Loading your GrimVault settings…"),
                  QSystemTrayIcon::Information, 5000);
               settings_sync.start ();
            }, Qt::QueuedConnection);
         });
         oauth_workers.insert (worker);
         QObject::connect (worker, &QThread::finished, &app, [&, worker] {
            oauth_workers.erase (worker);
            sign_in_active = false;
            worker->deleteLater ();
         });
         worker->start ();
      };

      QObject::connect (&tray, &gv::ui::TrayIcon::sign_in_requested, &app, do_sign_in);

      QObject::connect (&settings_sync, &gv::app::SettingsSync::poll_succeeded,
         &app, [&, settings_ready, settings_failure_notified] (int) {
            tray.set_connection_state (gv::ui::ConnectionState::Ready);
            *settings_failure_notified = false;
            if (*settings_ready) return;
            *settings_ready = true;
            tray.showMessage (QStringLiteral ("GrimVault ready"),
               QStringLiteral ("Your settings are synced and item analysis is ready."),
               QSystemTrayIcon::Information, 5000);
         });

      QObject::connect (&settings_sync, &gv::app::SettingsSync::poll_failed,
         &app, [&, settings_failure_notified] (const QString&) {
            if (!session.signed_in ()) return;
            tray.set_connection_state (gv::ui::ConnectionState::Degraded);
            if (*settings_failure_notified) return;
            *settings_failure_notified = true;
            tray.showMessage (QStringLiteral ("Settings temporarily unavailable"),
               QStringLiteral ("GrimVault is using safe local defaults and will retry automatically."),
               QSystemTrayIcon::Warning, 7000);
         });

      QObject::connect (&settings_sync, &gv::app::SettingsSync::authentication_required,
         &app, [&, signed_state, signed_subject, settings_ready,
                settings_failure_notified, do_sign_in] {
            settings_sync.stop ();
            *signed_state = false;
            signed_subject->clear ();
            *settings_ready = false;
            *settings_failure_notified = false;
            controller.set_authenticated (false);
            if (auto scoped = scope_settings (settings_repo, std::nullopt);
                scoped.has_value ()) {
               settings_bridge.reload ();
            } else {
               gv::core::log::app.error (
                  "settings sign-out scope failed: {}", scoped.error ().message);
            }
            tray.set_connection_state (gv::ui::ConnectionState::SignedOut);
            badge.set_signed_in (false);
            tray.showMessage (QStringLiteral ("Session expired"),
               QStringLiteral ("Please sign in again. Your local defaults remain safe."),
               QSystemTrayIcon::Warning, 7000);
            if (!opts.no_auto_login) QTimer::singleShot (0, &app, do_sign_in);
         });

      QObject::connect (&tray, &gv::ui::TrayIcon::sign_out_requested, &app, [&] {
         settings_sync.stop ();
         auto r = session.sign_out (/*local_only=*/ false);
         *signed_state = false;
         signed_subject->clear ();
         *settings_ready = false;
         *settings_failure_notified = false;
         controller.set_authenticated (false);
         if (auto scoped = scope_settings (settings_repo, std::nullopt);
             scoped.has_value ()) {
            settings_bridge.reload ();
         } else {
            gv::core::log::app.error (
               "settings sign-out scope failed: {}", scoped.error ().message);
         }
         tray.set_connection_state (gv::ui::ConnectionState::SignedOut);
         badge.set_signed_in (false);
         if (r.has_value ()) {
            tray.showMessage (QStringLiteral ("Signed out"),
               QStringLiteral ("Tokens cleared."),
               QSystemTrayIcon::Information, 4000);
         } else {
            tray.showMessage (QStringLiteral ("Sign-out failed"),
               QString::fromStdString (r.error ().message),
               QSystemTrayIcon::Warning, 6000);
         }
      });

      // Watch for sign-in state changes made outside this process — the CLI
      // (`grimvault login` / `logout`) writes the same token store but can't
      // signal us. Poll the session and reconcile the tray + settings sync
      // whenever the state flips.
      auto* auth_watch = new QTimer (&app);
      auth_watch->setInterval (10'000);
      QObject::connect (auth_watch, &QTimer::timeout, &app,
         [&, signed_state, signed_subject, settings_failure_notified] {

         session.reload ();
         const bool now = session.signed_in ();
         const auto principal = session.principal ();
         const auto subject = principal.value_or ("");
         const bool identity_changed = subject != *signed_subject;
         if (now == *signed_state && !identity_changed) return;

         settings_sync.stop ();
         controller.set_authenticated (false);
         if (identity_changed || !now) {
            auto scoped = scope_settings (settings_repo, principal);
            if (!scoped.has_value ()) {
               tray.set_connection_state (gv::ui::ConnectionState::Degraded);
               badge.set_signed_in (false);
               gv::core::log::app.error (
                  "settings external account scope failed: {}", scoped.error ().message);
               if (!*settings_failure_notified) {
                  *settings_failure_notified = true;
                  tray.showMessage (QStringLiteral ("Settings unavailable"),
                     QStringLiteral ("GrimVault paused to keep account settings isolated."),
                     QSystemTrayIcon::Critical, 8000);
               }
               return;
            }
            settings_bridge.reload ();
         }
         *signed_state = now;
         *signed_subject = subject;
         *settings_ready = false;
         *settings_failure_notified = false;

         gv::core::log::app.info ("auth state changed externally: {}",
            now ? "signed in" : "signed out");
         tray.set_connection_state (now
            ? gv::ui::ConnectionState::Syncing
            : gv::ui::ConnectionState::SignedOut);
         badge.set_signed_in (now);
         controller.set_authenticated (now, subject);

         if (now) {
            settings_sync.start ();
            tray.showMessage (QStringLiteral ("Signed in"),
               QStringLiteral ("Loading your GrimVault settings…"),
               QSystemTrayIcon::Information, 5000);
         }
      });
      auth_watch->start ();

      // Auto-launch the OAuth flow when starting up signed-out, unless the
      // operator passed --no-auto-login. Already-signed-in users skip
      // straight to settings sync.
      if (session.signed_in ()) {
         settings_sync.start ();
      } else if (opts.no_auto_login) {
         tray.showMessage (QStringLiteral ("GrimVault"),
            QStringLiteral ("Sign in via the tray to enable lookups. (Auto-login disabled.)"),
            QSystemTrayIcon::Information, 6000);
      } else {
         // Fire after the event loop is running so QMetaObject::invokeMethod
         // callbacks land cleanly.
         QTimer::singleShot (0, &app, do_sign_in);
      }

      QObject::connect (&tray, &gv::ui::TrayIcon::settings_requested, &app, [&active_env] {
         const QString url = QString::fromStdString (std::string { active_env.spa_base_url })
            + QStringLiteral ("/dashboard/grimvault");
         QDesktopServices::openUrl (QUrl (url));
      });
      QObject::connect (&tray, &gv::ui::TrayIcon::logs_requested, &app, [data_dir] {
         QDesktopServices::openUrl (QUrl::fromLocalFile (
            QString::fromStdString ((data_dir / "logs").string ())));
      });
      QObject::connect (&tray, &gv::ui::TrayIcon::check_updates_requested,
         &update_service, &gv::update::UpdateService::check_now_with_ui);
      QObject::connect (&tray, &gv::ui::TrayIcon::quit_requested,
         &app, &QApplication::quit);

      // The installer overwrites every loaded DLL, so the app must be gone
      // before WinSparkle runs it. Queued: the request arrives on a
      // WinSparkle worker thread.
      QObject::connect (&update_service, &gv::update::UpdateService::shutdown_requested,
         &app, &QApplication::quit, Qt::QueuedConnection);

      update_service.set_check_interval_seconds (3600);
      update_service.start ();

      // behavior:is_auto_update_enabled, gated by the env lock resolved above.
      // Re-evaluated on every settings change so toggling it in the
      // dashboard starts or stops the checker without a restart.
      auto running = std::make_shared<std::optional<bool>> ();
      auto sync_updates = [&update_service, &settings_bridge, disabled_by_env, running] {
         const bool want = settings_bridge.auto_updates_enabled ();

         if (running->has_value () && want == **running) return;
         *running = want;
         update_service.set_automatic_checks_enabled (want);

         if (want) {
            gv::core::log::update.info ("auto-updates enabled");
            return;
         }

         gv::core::log::update.info (disabled_by_env
            ? "skipped (GRIMVAULT_DISABLE_UPDATES set)"
            : "skipped (auto-updates disabled)");
      };

      QObject::connect (&settings_bridge, &gv::app::SettingsBridge::applied,
         &app, sync_updates);
      sync_updates ();

      const int rc = app.exec ();

      // OAuth owns a blocking loopback wait and HTTP exchange. Keep every
      // object captured by those workers alive and do not clean up libcurl
      // until the workers have returned.
      oauth->cancel_authorize ();
      for (auto* worker : oauth_workers) {
         worker->wait ();
         delete worker;
      }
      oauth_workers.clear ();

      settings_sync.stop ();
      controller.stop ();
      if (pipeline) pipeline->stop ();
      tracker.reset ();
      hotkeys.reset ();
      update_service.stop ();

      gv::core::Logger::info ("GrimVault exiting with code {}", rc);
      gv::core::Logger::shutdown ();
      return rc;
   }

   // ---- CLI run loop ----
   //
   // A QCoreApplication is required for QStandardPaths / QDesktopServices.
   // We construct it minimally and let the CLI dispatcher do its thing.
   int run_cli (int argc, char** argv, const std::vector<std::string>& cli_args)
   {
      attach_parent_console ();

      QCoreApplication app (argc, argv);
      app.setApplicationName    (QStringLiteral ("GrimVault"));
      app.setOrganizationName   (QStringLiteral ("DDB"));
      app.setOrganizationDomain (QStringLiteral ("darkerdb.com"));

      gv::core::http::Global http;
      if (!http) return 1;

      // CLI logging goes to file but not stdout so we don't crowd subcommand
      // output. Init quietly.
      auto data_dir = app_data_dir ();
      gv::core::Logger::init (data_dir / "logs", /*verbose=*/ false);

      const int rc = gv::cli::run (cli_args);

      gv::core::Logger::shutdown ();
      return rc;
   }

} // namespace

int main (int argc, char** argv)
{
   install_fatal_handlers ();
   enable_per_monitor_dpi ();

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
   for (int i = 1; i < argc; ++i) cli_args.emplace_back (argv [i]);

   // Resolve the active env up front so both GUI and CLI dispatch see the
   // same value. --env / --env=<name> is consumed from cli_args here.
   gv::core::set_active_env (gv::core::resolve_active_env (cli_args));

   const double fcr = consume_fcr (cli_args);

   const auto is_gui_flag = [] (const std::string& a) {
      return a == "--hidden" || a == "--no-auto-login"
          || a == "--debug"  || a.rfind ("--debug=", 0) == 0
          || a == "--detached"
          || a == "--detect-only";
   };

   const bool all_gui_flags = !cli_args.empty () &&
      std::all_of (cli_args.begin (), cli_args.end (), is_gui_flag);

   if (cli_args.empty () || all_gui_flags) {
#ifdef _WIN32
      if (std::find (cli_args.begin (), cli_args.end (), "--detached") != cli_args.end ()) {
         attach_parent_console ();
         return relaunch_detached (cli_args);
      }
#endif

      GuiOptions opts;
      opts.fcr = fcr;
      for (const auto& a : cli_args) {
         if (a == "--no-auto-login") opts.no_auto_login = true;
         if (a == "--debug" || a.rfind ("--debug=", 0) == 0) apply_debug_option (opts, a);
         if (a == "--detect-only")   opts.detect_only   = true;
      }
      return run_gui (argc, argv, opts);
   }
   return run_cli (argc, argv, cli_args);
}

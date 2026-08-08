#include <gv/api/darkerdb_client.h>
#include <gv/app/controller.h>
#include <gv/app/settings_sync.h>
#include <gv/auth/oauth_client.h>
#include <gv/auth/session.h>
#include <gv/capture/capture_service.h>
#include <gv/cli/cli.h>
#include <gv/core/crash_handler.h>
#include <gv/core/env.h>
#include <gv/core/env_resolver.h>
#include <gv/core/hotkey_manager.h>
#include <gv/core/ini_migrator.h>
#include <gv/core/logger.h>
#include <gv/core/single_instance.h>
#include <gv/core/startup_link.h>
#include <gv/core/version.h>
#include <gv/core/window_tracker.h>
#include <gv/db/database.h>
#include <gv/db/repos/user_hotkeys_repo.h>
#include <gv/db/repos/user_settings_repo.h>
#include <gv/ocr/language_registry.h>
#include <gv/ocr/pipeline.h>
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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

   constexpr const char* k_single_instance_mutex = "GrimVault.SingleInstance.v1";
   constexpr const char* k_surface_message       = "GrimVault.Surface.v1";

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
      auto qpath = QStandardPaths::writableLocation (QStandardPaths::AppDataLocation);
      QDir ().mkpath (qpath);
      return std::filesystem::path { qpath.toStdWString () };
   }

   std::filesystem::path resolve_install_dir ()
   {
      if (const char* env = std::getenv ("GRIMVAULT_DEV_RESOURCES"); env && *env) {
         return std::filesystem::path { env };
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
      bool no_auto_login = false;
      bool debug         = false;
   };

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

      gv::api::DarkerDbClient::global_init ();

      QApplication app (argc, argv);
      app.setApplicationName    (QStringLiteral ("GrimVault"));
      app.setOrganizationName   (QStringLiteral ("DarkerDB"));
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
#endif
         gv::core::Logger::info ("debug mode: verbose logs + OCR stage dumps enabled");
      }

      const auto& active_env = gv::core::active_env ();
      gv::core::Logger::info ("GrimVault {} (env={}, api={}, auth={})",
         gv::core::version::string,
         std::string { active_env.name },
         std::string { active_env.api_base_url },
         std::string { active_env.auth_base_url });
      if (active_env.name == "dev") {
         gv::core::Logger::warn (
            "TLS verification disabled for env=dev (self-signed certs allowed)");
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

      // ---- Overlay + capture pipeline ----
      gv::ui::OverlayWindow::Config overlay_cfg {
         .web_dir       = install_dir / "web",
         .user_data_dir = data_dir / "webview2",
      };
      if (auto v = settings_repo.get ("overlay:renderer"); v.has_value () && v->has_value ()) {
         overlay_cfg.renderer = **v;
      }
      gv::ui::OverlayWindow overlay { std::move (overlay_cfg) };

      gv::api::DarkerDbClient::Config api_cfg;
      api_cfg.base_url  = std::string { active_env.api_base_url };
      api_cfg.client_id = std::string { active_env.client_id };
      gv::api::DarkerDbClient api_client { api_cfg, &session, db->get () };

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

      std::unique_ptr<gv::ocr::Pipeline> pipeline;
      if (capture) {
         pipeline = std::make_unique<gv::ocr::Pipeline> (
            *capture, detector, langs, gv::ocr::Pipeline::Config {});
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
      };
      gv::app::Controller controller { deps };
      if (opts.debug) controller.action_debug_toggle ();

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

      const int bound = controller.bind_hotkeys_from_repo ();
      gv::core::Logger::info ("hotkeys: {} bindings active", bound);

      // Apply login-on-startup setting (HKCU\...\Run).
      if (auto v = settings_repo.get ("general:launch_on_startup"); v.has_value () && v->has_value ()) {
         const bool want = (**v == "true" || **v == "1");
         if (want) {
            gv::core::StartupLink::enable ("GrimVault",
               app.applicationFilePath ().toStdString (), "--hidden");
         } else {
            gv::core::StartupLink::disable ("GrimVault");
         }
      }

      // ---- Tray icon ----
      if (!QSystemTrayIcon::isSystemTrayAvailable ()) {
         gv::core::Logger::error ("system tray not available; exiting");
         return 1;
      }

      gv::ui::TrayIcon tray;
      tray.set_signed_in (session.signed_in ());

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
      gv::app::SettingsSync settings_sync { &api_client, &session, &settings_repo };
      QObject::connect (&settings_sync, &gv::app::SettingsSync::settings_changed,
         &app, [] (const QString& key, const QString& value) {
            gv::core::log::app.info ("settings updated: {} = {}",
               key.toStdString (), value.toStdString ());
         });

      // Per contract §7.2: presence of settings.toml IS the first-run flag.
      // We drop a header-only stub on first launch so subsequent launches
      // know the user has been here before. Deleting the file resets state.
      //
      // The stub's contents will be replaced wholesale by the first-run
      // wizard's confirmed values (hotkey + capture region) when that
      // surface is built post-MVP.
      const auto settings_path = data_dir / "settings.toml";
      const bool first_run = !std::filesystem::exists (settings_path);
      if (first_run) {
         std::ofstream f { settings_path };
         if (f) {
            f << "# GrimVault settings — written on first launch.\n"
              << "# Defaults apply for any key not set here.\n"
              << "# See docs/architecture/grimvault-mvp.md §7.2 for the schema.\n";
         } else {
            gv::core::Logger::warn ("first-run: could not create {}",
               settings_path.string ());
         }
      }

      // Canonical signed-in state as the GUI last reconciled it. Updated by
      // the in-process sign-in/out handlers below and by the external-change
      // watcher, so neither double-reacts to a flip the other already handled.
      auto signed_state = std::make_shared<bool> (session.signed_in ());

      // ---- Sign-in: run the OAuth flow off the Qt main thread so the UI
      // stays responsive while the loopback server blocks on the callback.
      auto do_sign_in = [&] {
         tray.showMessage (QStringLiteral ("GrimVault"),
            QStringLiteral ("Opening your browser to sign in…"),
            QSystemTrayIcon::Information, 4000);

         auto* worker = QThread::create ([&] {
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
               *signed_state = true;
               tray.set_signed_in (true);
               badge.set_signed_in (true);
               tray.showMessage (QStringLiteral ("Signed in"),
                  QStringLiteral ("GrimVault is now connected to DarkerDB."),
                  QSystemTrayIcon::Information, 5000);
               settings_sync.start ();
            }, Qt::QueuedConnection);
         });
         QObject::connect (worker, &QThread::finished, worker, &QObject::deleteLater);
         worker->start ();
      };

      QObject::connect (&tray, &gv::ui::TrayIcon::sign_in_requested, &app, do_sign_in);

      QObject::connect (&tray, &gv::ui::TrayIcon::sign_out_requested, &app, [&] {
         settings_sync.stop ();
         auto r = session.sign_out (/*local_only=*/ false);
         *signed_state = false;
         tray.set_signed_in (false);
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
      QObject::connect (auth_watch, &QTimer::timeout, &app, [&, signed_state] {

         const bool now = session.signed_in ();
         if (now == *signed_state) return;
         *signed_state = now;

         gv::core::log::app.info ("auth state changed externally: {}",
            now ? "signed in" : "signed out");
         tray.set_signed_in (now);
         badge.set_signed_in (now);

         if (now) {
            settings_sync.start ();
            tray.showMessage (QStringLiteral ("Signed in"),
               QStringLiteral ("GrimVault is now connected to DarkerDB."),
               QSystemTrayIcon::Information, 5000);
         } else {
            settings_sync.stop ();
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

      // Env flag wins over the user setting so dev/CI can suppress unconditionally.
      const char* dis_env = std::getenv ("GRIMVAULT_DISABLE_UPDATES");
      const bool  disabled_by_env = dis_env && *dis_env && std::string_view { dis_env } != "0";

      bool auto_updates = true;
      if (auto v = settings_repo.get ("general:auto_updates"); v.has_value () && v->has_value ()) {
         auto_updates = (**v == "true" || **v == "1");
      }

      if (auto_updates && !disabled_by_env) {
         update_service.set_check_interval_seconds (3600);
         update_service.start ();
      } else if (disabled_by_env) {
         gv::core::log::update.info ("skipped (GRIMVAULT_DISABLE_UPDATES set)");
      } else {
         gv::core::log::update.info ("skipped (auto_updates disabled)");
      }

      const int rc = app.exec ();

      settings_sync.stop ();
      if (pipeline) pipeline->stop ();
      tracker.reset ();
      hotkeys.reset ();
      update_service.stop ();

      gv::core::Logger::info ("GrimVault exiting with code {}", rc);
      gv::core::Logger::shutdown ();
      gv::api::DarkerDbClient::global_cleanup ();
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
      app.setOrganizationName   (QStringLiteral ("DarkerDB"));
      app.setOrganizationDomain (QStringLiteral ("darkerdb.com"));

      gv::api::DarkerDbClient::global_init ();

      // CLI logging goes to file but not stdout so we don't crowd subcommand
      // output. Init quietly.
      auto data_dir = app_data_dir ();
      gv::core::Logger::init (data_dir / "logs", /*verbose=*/ false);

      const int rc = gv::cli::run (cli_args);

      gv::core::Logger::shutdown ();
      gv::api::DarkerDbClient::global_cleanup ();
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
   // logs + OCR stage dumps + debug overlay), --detached (relaunch in the
   // background and return immediately).
   std::vector<std::string> cli_args;
   for (int i = 1; i < argc; ++i) cli_args.emplace_back (argv [i]);

   // Resolve the active env up front so both GUI and CLI dispatch see the
   // same value. --env / --env=<name> is consumed from cli_args here.
   gv::core::set_active_env (gv::core::resolve_active_env (cli_args));

   const auto is_gui_flag = [] (const std::string& a) {
      return a == "--hidden" || a == "--no-auto-login"
          || a == "--debug"  || a == "--detached";
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
      for (const auto& a : cli_args) {
         if (a == "--no-auto-login") opts.no_auto_login = true;
         if (a == "--debug")         opts.debug         = true;
      }
      return run_gui (argc, argv, opts);
   }
   return run_cli (argc, argv, cli_args);
}

#include <gv/api/darkerdb_client.h>
#include <gv/app/controller.h>
#include <gv/auth/oauth_client.h>
#include <gv/auth/session.h>
#include <gv/capture/capture_service.h>
#include <gv/cli/cli.h>
#include <gv/core/crash_handler.h>
#include <gv/core/env.h>
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
   #include <io.h>
   #include <fcntl.h>
#endif

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
   // shows a console flash. CLI invocations need explicit AttachConsole +
   // stdout/stderr reopen so output makes it back to the parent terminal.
   void attach_parent_console ()
   {
#ifdef _WIN32
      if (::AttachConsole (ATTACH_PARENT_PROCESS)) {
         FILE* dummy = nullptr;
         freopen_s (&dummy, "CONOUT$", "w", stdout);
         freopen_s (&dummy, "CONOUT$", "w", stderr);
         freopen_s (&dummy, "CONIN$",  "r", stdin);
         std::ios::sync_with_stdio ();
      }
#endif
   }

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
      QFontDatabase::addApplicationFont (QStringLiteral (":/assets/fonts/SaintKDG_Light.ttf"));
      QFontDatabase::addApplicationFont (QStringLiteral (":/assets/fonts/SaintKDG_Medium.ttf"));
      QFontDatabase::addApplicationFont (QStringLiteral (":/assets/fonts/Pelagiad.ttf"));
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

   int run_gui (int argc, char** argv)
   {
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

      const auto data_dir = app_data_dir ();

      gv::core::Logger::init (data_dir / "logs", /*verbose=*/ false);
      qInstallMessageHandler (&qt_message_handler);
      gv::core::Logger::info ("GrimVault {} (env={}, api={})",
         gv::core::version::string,
         gv::core::env,
         gv::core::api_base_url);

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
      oauth_cfg.client_id    = gv::core::client_id;
      oauth_cfg.api_base_url = gv::core::api_base_url;
      oauth_cfg.spa_base_url = gv::core::spa_base_url;
      auto oauth = std::make_shared<gv::auth::OauthClient> (std::move (oauth_cfg));

      gv::auth::Session session { oauth };

      // ---- Overlay + capture pipeline ----
      gv::ui::OverlayWindow overlay;

      gv::api::DarkerDbClient::Config api_cfg;
      api_cfg.base_url  = gv::core::api_base_url;
      api_cfg.client_id = gv::core::client_id;
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

      // Per contract §7.2: presence of settings.toml IS the first-run flag.
      // We greet on first launch, then drop a header-only stub so subsequent
      // launches skip the welcome notification regardless of whether the
      // user actually signed in. Returning users (file exists) get the
      // narrower "sign in to enable lookups" nudge iff they're still
      // signed out. Deleting the file resets the first-run state.
      //
      // The stub's contents will be replaced wholesale by the first-run
      // wizard's confirmed values (hotkey + capture region) when that
      // surface is built post-MVP.
      const auto settings_path = data_dir / "settings.toml";
      const bool first_run = !std::filesystem::exists (settings_path);
      if (first_run) {
         tray.showMessage (QStringLiteral ("GrimVault"),
            QStringLiteral ("Welcome — right-click the tray icon to sign in."),
            QSystemTrayIcon::Information, 6000);

         std::ofstream f { settings_path };
         if (f) {
            f << "# GrimVault settings — written on first launch.\n"
              << "# Defaults apply for any key not set here.\n"
              << "# See docs/architecture/grimvault-mvp.md §7.2 for the schema.\n";
         } else {
            gv::core::Logger::warn ("first-run: could not create {}",
               settings_path.string ());
         }
      } else if (!session.signed_in ()) {
         tray.showMessage (QStringLiteral ("GrimVault"),
            QStringLiteral ("Sign in to DarkerDB to enable price lookups."),
            QSystemTrayIcon::Information, 6000);
      }

      // ---- Sign-in: run the OAuth flow off the Qt main thread so the UI
      // stays responsive while the loopback server blocks on the callback.
      QObject::connect (&tray, &gv::ui::TrayIcon::sign_in_requested, &app, [&] {
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
               tray.set_signed_in (true);
               tray.showMessage (QStringLiteral ("Signed in"),
                  QStringLiteral ("GrimVault is now connected to DarkerDB."),
                  QSystemTrayIcon::Information, 5000);
            }, Qt::QueuedConnection);
         });
         QObject::connect (worker, &QThread::finished, worker, &QObject::deleteLater);
         worker->start ();
      });

      QObject::connect (&tray, &gv::ui::TrayIcon::sign_out_requested, &app, [&] {
         auto r = session.sign_out (/*local_only=*/ false);
         tray.set_signed_in (false);
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

      QObject::connect (&tray, &gv::ui::TrayIcon::open_dashboard_requested, &app, [] {
         const QString url = QString::fromLatin1 (gv::core::spa_base_url)
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
   enable_per_monitor_dpi ();

   // Force the static-lib .qrc initializers to link in. Without this, MSVC's
   // linker strips the resources' anonymous-namespace init from the static
   // libs because nothing else references them.
   Q_INIT_RESOURCE (qml);
   Q_INIT_RESOURCE (auth);

   // Dual-mode dispatch:
   //    - argc == 1 → GUI mode (tray + overlay)
   //    - argc >  1 → CLI mode (subcommand + flags)
   //
   // The one wrinkle: the OS launches us with "--hidden" when the autostart
   // entry fires. Treat that as GUI mode too.
   std::vector<std::string> cli_args;
   for (int i = 1; i < argc; ++i) cli_args.emplace_back (argv [i]);

   const bool gui_only_flag = (cli_args.size () == 1 && cli_args [0] == "--hidden");
   if (cli_args.empty () || gui_only_flag) {
      return run_gui (argc, argv);
   }
   return run_cli (argc, argv, cli_args);
}

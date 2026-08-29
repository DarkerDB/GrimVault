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
#include <QIcon>
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
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace gv::app {

namespace {

constexpr const char* k_single_instance_mutex = "GrimVault.SingleInstance.v1";
constexpr const char* k_surface_message = "GrimVault.Surface.v1";

bool env_enabled (const char* name)
{
   const auto value = qEnvironmentVariable (name);
   return !value.isEmpty () && value != "0";
}

// ---- console attach for CLI on Windows ----
void attach_console_impl ()
{
#ifdef _WIN32
   auto redirected = [] (DWORD std_handle) {
      HANDLE h = ::GetStdHandle (std_handle);
      return h != nullptr && h != INVALID_HANDLE_VALUE && ::GetFileType (h) != FILE_TYPE_UNKNOWN;
   };

   const bool out_redirected = redirected (STD_OUTPUT_HANDLE);
   const bool err_redirected = redirected (STD_ERROR_HANDLE);
   const bool in_redirected = redirected (STD_INPUT_HANDLE);

   if (::AttachConsole (ATTACH_PARENT_PROCESS)) {
      FILE* dummy = nullptr;

      if (!out_redirected) freopen_s (&dummy, "CONOUT$", "w", stdout);
      if (!err_redirected) freopen_s (&dummy, "CONOUT$", "w", stderr);
      if (!in_redirected) freopen_s (&dummy, "CONIN$", "r", stdin);
      std::ios::sync_with_stdio ();
   }
#endif
}

#ifdef _WIN32
BOOL WINAPI console_ctrl_handler (DWORD type)
{
   switch (type) {
      case CTRL_C_EVENT:
      case CTRL_BREAK_EVENT:
         if (auto* app = QCoreApplication::instance ()) {
            QMetaObject::invokeMethod (app, &QCoreApplication::quit, Qt::QueuedConnection);
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
   auto qpath = QStandardPaths::writableLocation (QStandardPaths::GenericDataLocation) +
                QStringLiteral ("/GrimVault");
   const auto env = gv::core::active_env ().name;
   if (env != "prod") {
      qpath += QStringLiteral ("/") +
               QString::fromUtf8 (env.data (), static_cast<qsizetype> (env.size ()));
   }
   QDir ().mkpath (qpath);
   return std::filesystem::path { qpath.toStdWString () };
}

gv::core::Result<bool> scope_settings (gv::db::UserSettingsRepo& repo,
                                       const std::optional<std::string>& principal)
{
   constexpr std::string_view marker = "auth:subject";
   auto stored = repo.get (marker);
   if (!stored.has_value ()) return gv::core::fail (stored.error ());

   const bool has_marker = stored.has_value () && stored->has_value ();
   const std::string next = principal.value_or ("");
   const std::string current = has_marker ? **stored : "";

   if (!has_marker && !next.empty ()) {
      auto saved = repo.set (std::string { marker }, next);
      if (!saved.has_value ()) return gv::core::fail (saved.error ());
      return false;
   }
   if (has_marker && current == next) return false;

   auto all = repo.all ();
   if (!all.has_value ()) return gv::core::fail (all.error ());

   for (const auto& [key, value] : *all) {
      (void)value;
      if (!gv::app::is_managed_setting (key)) continue;

      auto erased = repo.erase (key);
      if (!erased.has_value ()) return gv::core::fail (erased.error ());
   }

   auto marked =
      next.empty () ? repo.erase (std::string { marker }) : repo.set (std::string { marker }, next);
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

void enable_dpi_impl ()
{
#ifdef _WIN32
   using SetCtxFn = BOOL (WINAPI*) (DPI_AWARENESS_CONTEXT);

   if (auto* mod = ::GetModuleHandleW (L"user32.dll")) {
      if (auto fn =
             reinterpret_cast<SetCtxFn> (::GetProcAddress (mod, "SetProcessDpiAwarenessContext"))) {
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
      gv::core::Logger::info (
         "fonts: {} -> [{}]", path,
         id >= 0 ? QFontDatabase::applicationFontFamilies (id).join (", ").toStdString ()
                 : std::string { "LOAD FAILED" });
   }
}

void qt_message_handler (QtMsgType type, const QMessageLogContext& ctx, const QString& msg)
{
   const std::string txt = msg.toStdString ();
   const std::string where =
      ctx.category ? std::string { "qt." } + ctx.category : std::string { "qt" };

   switch (type) {
      case QtDebugMsg:
         gv::core::Logger::debug ("{}: {}", where, txt);
         break;
      case QtInfoMsg:
         gv::core::Logger::info ("{}: {}", where, txt);
         break;
      case QtWarningMsg:
         gv::core::Logger::warn ("{}: {}", where, txt);
         break;
      case QtCriticalMsg:
         gv::core::Logger::error ("{}: {}", where, txt);
         break;
      case QtFatalMsg:
         gv::core::Logger::error ("{}: {} (fatal)", where, txt);
         break;
   }
}

int run_gui_impl (int argc, char** argv, GuiOptions opts)
{
   attach_console_impl ();

   auto single_instance = gv::core::SingleInstanceGuard::acquire (k_single_instance_mutex);
   if (!single_instance) {
      gv::core::SingleInstanceGuard::notify_existing (k_surface_message);
      return 0;
   }

   gv::core::http::Global http;
   if (!http) return 1;

   qputenv ("QSG_RENDER_LOOP", "basic");

   QApplication app (argc, argv);
   app.setApplicationName (QStringLiteral ("GrimVault"));
   app.setOrganizationName (QStringLiteral ("DDB"));
   app.setOrganizationDomain (QStringLiteral ("darkerdb.com"));
   app.setApplicationVersion (QString::fromLatin1 (gv::core::version::string));
   app.setQuitOnLastWindowClosed (false);
   app.setWindowIcon (QIcon (QStringLiteral (":/assets/images/Icon-324x356.png")));

#ifdef _WIN32
   ::SetConsoleCtrlHandler (&console_ctrl_handler, TRUE);
#endif

   const auto data_dir = app_data_dir ();

   gv::core::Logger::init (data_dir / "logs", opts.debug);
   qInstallMessageHandler (&qt_message_handler);

   if (opts.debug) {
#ifdef _WIN32
      ::_putenv_s ("GRIMVAULT_OCR_DEBUG", "1");
      ::_putenv_s ("GRIMVAULT_ANCHOR_DIAGNOSTICS",
                   (data_dir / "logs" / "anchoring").string ().c_str ());
#endif
      gv::core::Logger::info ("debug mode: verbose logs + OCR stage dumps enabled");
      gv::core::Logger::info ("debug highlights: objects={}, game={}", opts.highlight_objects,
                              opts.highlight_game);
   }

   const auto& active_env = gv::core::active_env ();
   gv::core::Logger::info ("GrimVault {} (env={}, api={}, auth={})", gv::core::version::string,
                           std::string { active_env.name }, std::string { active_env.api_base_url },
                           std::string { active_env.auth_base_url });
   if (active_env.name == "dev" && env_enabled ("GRIMVAULT_INSECURE_DEV_TLS")) {
      gv::core::Logger::warn ("TLS verification explicitly disabled for env=dev");
   }

   gv::core::CrashHandler::install (data_dir / "logs");

   auto db = gv::db::Database::open (data_dir / "grimvault.db");
   if (!db.has_value ()) {
      gv::core::Logger::error ("Failed to open database: {}", db.error ().message);
      return 1;
   }

   gv::db::UserSettingsRepo settings_repo { **db };
   gv::db::UserHotkeysRepo hotkeys_repo { **db };

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
   oauth_cfg.client_id = std::string { active_env.client_id };
   oauth_cfg.api_base_url = std::string { active_env.api_base_url };
   oauth_cfg.auth_base_url = std::string { active_env.auth_base_url };
   oauth_cfg.spa_base_url = std::string { active_env.spa_base_url };
   auto oauth = std::make_shared<gv::auth::OauthClient> (std::move (oauth_cfg));

   gv::auth::Session session { oauth };
   if (auto scoped = scope_settings (settings_repo, session.principal ()); !scoped.has_value ()) {
      gv::core::Logger::error ("Settings account scope failed: {}", scoped.error ().message);
      return 1;
   }

   // ---- Overlay + capture pipeline ----
   auto web_dir = install_dir / "web";
   if (const auto staged =
          std::filesystem::path { QCoreApplication::applicationDirPath ().toStdWString () } / "web";
       std::filesystem::exists (staged / "augment.html")) {
      web_dir = staged;
   }

   gv::ui::OverlayWindow::Config overlay_cfg {
      .web_dir = std::move (web_dir),
      .user_data_dir = data_dir / "webview2",
   };
   gv::ui::OverlayWindow overlay { std::move (overlay_cfg) };
   gv::ui::DebugOverlay debug_overlay;

   gv::api::DDBClient::Config api_cfg;
   api_cfg.base_url = std::string { active_env.api_base_url };
   api_cfg.collection_base_url = std::string { active_env.collection_base_url };
   api_cfg.client_id = std::string { active_env.client_id };
   gv::api::DDBClient api_client { api_cfg, &session, db->get () };
   gv::collection::Collector collection { api_client };
   const auto collection_install_id = gv::core::diagnostics::install_id (data_dir);

   gv::vision::TooltipDetector detector;
   if (auto r =
          detector.initialize (install_dir / "models" / gv::vision::model_files::tooltip_full);
       !r.has_value ()) {
      gv::core::Logger::warn ("vision: tooltip detector init failed: {}", r.error ().message);
   }

   gv::ocr::LanguageRegistry langs { install_dir / "models", 2 };

   std::unique_ptr<gv::capture::CaptureService> capture;
   if (auto cs = gv::capture::CaptureService::create (); cs.has_value ()) {
      capture = std::move (*cs);
      gv::core::Logger::info ("capture: probe selected {}",
                              std::string { capture->current ().name () });
   } else {
      gv::core::Logger::warn ("capture: not available at startup ({})", cs.error ().message);
   }

   if (capture) {
      if (auto v = settings_repo.get ("behavior:capture_mode"); v.has_value () && v->has_value ()) {
         if (const auto mode = gv::capture::parse_capture_mode (**v); mode.has_value ()) {
            if (auto r = capture->set_mode (*mode); !r.has_value ()) {
               gv::core::Logger::warn ("capture: stored mode {} rejected: {}", **v,
                                       r.error ().message);
            }
         }
      }
   }

   const auto publish_session_header = [&] {
      namespace diag = gv::core::diagnostics;

      std::vector<std::string> header;
      header.push_back (fmt::format ("grimvault {} env={} session={} install={}",
                                     gv::core::version::string, std::string { active_env.name },
                                     diag::session_id (), diag::install_id (data_dir)));
      header.push_back (fmt::format ("api={} auth={}", std::string { active_env.api_base_url },
                                     std::string { active_env.auth_base_url }));
      header.push_back (fmt::format ("identity={}", session.principal ().value_or ("signed-out")));
      header.push_back (fmt::format (
         "capture={}", capture ? std::string { capture->current ().name () } : "none"));

      for (auto& line : diag::machine ()) header.push_back (std::move (line));

      if (auto stored = settings_repo.all (); stored.has_value ()) {
         std::vector<std::string> keys;
         keys.reserve (stored->size ());
         for (const auto& [key, value] : *stored) {
            keys.push_back (fmt::format ("{}={}", key, value));
         }
         std::sort (keys.begin (), keys.end ());
         for (auto& entry : keys) {
            header.push_back (fmt::format ("setting {}", entry));
         }
      }

      gv::core::Logger::set_header (std::move (header));
   };

   publish_session_header ();

   std::unique_ptr<gv::ocr::Pipeline> pipeline;
   if (capture) {
      gv::ocr::Pipeline::Config pipe_cfg;
      if (opts.fcr > 0.0) {
         pipe_cfg.capture_fps = opts.fcr;
         gv::core::Logger::info ("pipeline: frame capture rate {} fps (--fcr)", opts.fcr);
      }
      if (opts.debug) {
         pipe_cfg.evidence_dir = data_dir / "evidence";
         gv::core::Logger::info ("Evidence directory: {}", pipe_cfg.evidence_dir.string ());
      }
      pipeline = std::make_unique<gv::ocr::Pipeline> (*capture, detector, langs, pipe_cfg);
      pipeline->on_sample ([&collection, &collection_install_id] (gv::ocr::TooltipSample sample) {
         if (!collection.enabled ()) return;
         std::vector<unsigned char> png;
         if (!cv::imencode (".png", sample.image, png)) return;
         collection.submit ({
               .channel = "tooltip",
               .content_type = "image/png",
               .body = std::string { reinterpret_cast<const char*> (png.data ()), png.size () },
               .metadata = {
                  { "schema", 1 },
                  { "install_id", collection_install_id },
                  { "generation", sample.generation },
                  { "locale", sample.locale },
                  { "prediction", sample.text },
                  { "rarity", sample.rarity },
                  { "confidence", sample.confidence },
                  { "capture_backend", std::string { gv::capture::backend_name (sample.backend) } },
                  { "rect", {
                     { "x", sample.rect.x },
                     { "y", sample.rect.y },
                     { "width", sample.rect.w },
                     { "height", sample.rect.h },
                  } },
               },
            });
      });

      if (opts.detect_only) {
         pipeline->set_detect_only (true);
         gv::core::Logger::info ("pipeline: detect-only mode (OCR / lookup / augment disabled)");
      } else if (opts.ocr_only) {
         gv::core::Logger::info ("pipeline: OCR-only mode (lookup / augment disabled)");
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
      .db = db->get (),
      .hotkeys_repo = &hotkeys_repo,
      .settings_repo = &settings_repo,
      .api = opts.ocr_only ? nullptr : &api_client,
      .pipeline = pipeline.get (),
      .hotkeys = hotkeys.get (),
      .overlay = &overlay,
      .debug = &debug_overlay,
      .highlight_game = opts.highlight_game,
      .highlight_objects = opts.debug,
   };
   gv::app::Controller controller { deps };
   collection.on_uploaded ([&debug_overlay] (const gv::api::CollectionSample& sample) {
      if (sample.channel != "tooltip") return;
      const auto generation = sample.metadata.value ("generation", std::uint64_t { 0 });
      if (generation == 0) return;
      QMetaObject::invokeMethod (
         &debug_overlay, [&debug_overlay, generation] { debug_overlay.mark_uploaded (generation); },
         Qt::QueuedConnection);
   });
   controller.set_authenticated (session.signed_in (), session.principal ().value_or (""));

   std::unique_ptr<gv::core::WindowTracker> tracker;
   if (auto t = gv::core::WindowTracker::create (
          {}, [&controller] (const gv::core::WindowEvent& ev) { controller.on_window_event (ev); });
       t.has_value ()) {
      tracker = std::move (*t);
   } else {
      gv::core::Logger::warn ("window_tracker init failed: {}", t.error ().message);
   }

   if (pipeline) {
      auto start_r = pipeline->start (
         [&controller] (const gv::ocr::RecognizedTooltip& rt) { controller.on_tooltip (rt); });
      if (!start_r.has_value ()) {
         gv::core::Logger::error ("pipeline: start failed: {}", start_r.error ().message);
      }
   }

   controller.set_browse_base (std::string { active_env.spa_base_url });

   const int bound = controller.bind_hotkeys_from_repo ();
   gv::core::Logger::info ("hotkeys: {} bindings active", bound);

   const bool disabled_by_env = env_enabled ("GRIMVAULT_DISABLE_UPDATES");

   // ---- Settings application ----
   gv::app::SettingsBridge settings_bridge { {
      .repo = &settings_repo,
      .overlay = &overlay,
      .controller = &controller,
      .collection = nullptr,
      .exe_path = app.applicationFilePath ().toStdString (),
      .updates_locked_off = disabled_by_env,
      .capture_fps_locked = opts.fcr > 0.0,
   } };
   settings_bridge.reload ();

   const auto collect_log = [&collection, &collection_install_id, &data_dir, &active_env] {
      gv::collection::submit_latest_log (collection, data_dir / "logs", collection_install_id,
                                         gv::core::version::string, active_env.name);
   };
   QTimer log_collection_timer;
   QObject::connect (&log_collection_timer, &QTimer::timeout, &app, collect_log);
   log_collection_timer.start (std::chrono::hours { 12 });

   // ---- Tray icon ----
   if (!QSystemTrayIcon::isSystemTrayAvailable ()) {
      gv::core::Logger::error ("system tray not available; exiting");
      return 1;
   }

   gv::ui::TrayIcon tray;
   QObject::connect (
      &overlay, &gv::ui::OverlayWindow::renderer_failed, &tray, [&tray] (const QString&) {
         tray.showMessage (QStringLiteral ("Overlay unavailable"),
                           QStringLiteral ("WebView2 could not start after automatic recovery. "
                                           "Open Logs from the tray for diagnostics."),
                           QSystemTrayIcon::Critical, 10000);
      });
   tray.set_connection_state (session.signed_in () ? gv::ui::ConnectionState::Syncing
                                                   : gv::ui::ConnectionState::SignedOut);

   gv::ui::StatusBadge badge;
   badge.set_signed_in (session.signed_in ());
   QObject::connect (&controller, &gv::app::Controller::gameWindowChanged, &badge,
                     &gv::ui::StatusBadge::set_game);
   QObject::connect (&controller, &gv::app::Controller::overlayPresented, &badge,
                     &gv::ui::StatusBadge::pulse);
   QObject::connect (&controller, &gv::app::Controller::modeChanged, &badge,
                     [&badge] (gv::app::Mode m) { badge.set_auto (m == gv::app::Mode::Auto); });

   auto sync_badge = [&badge, &controller, &settings_bridge] {
      badge.set_enabled (settings_bridge.preferences ().indicator_visible);
      badge.set_locale (controller.language ());
   };
   QObject::connect (&settings_bridge, &gv::app::SettingsBridge::applied, &app, sync_badge);
   sync_badge ();

   // ---- Settings sync ----
   gv::app::SettingsSync::Config sync_cfg;
   if (active_env.name == "dev") {
      sync_cfg.interval = std::chrono::seconds { 5 };
      sync_cfg.backoff_floor = std::chrono::seconds { 5 };
   }

   gv::app::SettingsSync settings_sync { &api_client, &session, &settings_repo, sync_cfg };
   QObject::connect (&settings_sync, &gv::app::SettingsSync::settings_changed, &settings_bridge,
                     &gv::app::SettingsBridge::apply);

   auto signed_state = std::make_shared<bool> (session.signed_in ());
   auto signed_subject = std::make_shared<std::string> (session.principal ().value_or (""));
   auto settings_ready = std::make_shared<bool> (false);
   auto settings_failure_notified = std::make_shared<bool> (false);
   std::unordered_set<QThread*> oauth_workers;
   bool sign_in_active = false;

   // ---- Sign-in ----
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
         QMetaObject::invokeMethod (
            &app,
            [&, resp = std::move (resp)] () mutable {
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
                  (void)session.sign_out (true);
                  tray.showMessage (QStringLiteral ("Sign-in failed"),
                                    QStringLiteral ("Could not isolate settings for this account."),
                                    QSystemTrayIcon::Critical, 8000);
                  gv::core::log::app.error ("settings account scope failed: {}",
                                            scoped.error ().message);
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
            },
            Qt::QueuedConnection);
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

   QObject::connect (
      &settings_sync, &gv::app::SettingsSync::poll_succeeded, &app,
      [&, settings_ready, settings_failure_notified] (int, bool improvement_enabled) {
         collection.set_enabled (improvement_enabled);
         collect_log ();
         tray.set_connection_state (gv::ui::ConnectionState::Ready);
         *settings_failure_notified = false;
         if (*settings_ready) return;
         *settings_ready = true;
         tray.showMessage (QStringLiteral ("GrimVault ready"),
                           QStringLiteral ("Your settings are synced and item analysis is ready."),
                           QSystemTrayIcon::Information, 5000);
      });

   QObject::connect (
      &settings_sync, &gv::app::SettingsSync::poll_failed, &app,
      [&, settings_failure_notified] (const QString&) {
         if (!session.signed_in ()) return;
         tray.set_connection_state (gv::ui::ConnectionState::Degraded);
         if (*settings_failure_notified) return;
         *settings_failure_notified = true;
         tray.showMessage (
            QStringLiteral ("Settings temporarily unavailable"),
            QStringLiteral ("GrimVault is using safe local defaults and will retry automatically."),
            QSystemTrayIcon::Warning, 7000);
      });

   QObject::connect (
      &settings_sync, &gv::app::SettingsSync::authentication_required, &app,
      [&, signed_state, signed_subject, settings_ready, settings_failure_notified, do_sign_in] {
         settings_sync.stop ();
         collection.set_enabled (false);
         *signed_state = false;
         signed_subject->clear ();
         *settings_ready = false;
         *settings_failure_notified = false;
         controller.set_authenticated (false);
         if (auto scoped = scope_settings (settings_repo, std::nullopt); scoped.has_value ()) {
            settings_bridge.reload ();
         } else {
            gv::core::log::app.error ("settings sign-out scope failed: {}",
                                      scoped.error ().message);
         }
         tray.set_connection_state (gv::ui::ConnectionState::SignedOut);
         badge.set_signed_in (false);
         tray.showMessage (
            QStringLiteral ("Session expired"),
            QStringLiteral ("Please sign in again. Your local defaults remain safe."),
            QSystemTrayIcon::Warning, 7000);
         if (!opts.no_auto_login) QTimer::singleShot (0, &app, do_sign_in);
      });

   QObject::connect (&tray, &gv::ui::TrayIcon::sign_out_requested, &app, [&] {
      settings_sync.stop ();
      collection.set_enabled (false);
      auto r = session.sign_out (false);
      *signed_state = false;
      signed_subject->clear ();
      *settings_ready = false;
      *settings_failure_notified = false;
      controller.set_authenticated (false);
      if (auto scoped = scope_settings (settings_repo, std::nullopt); scoped.has_value ()) {
         settings_bridge.reload ();
      } else {
         gv::core::log::app.error ("settings sign-out scope failed: {}", scoped.error ().message);
      }
      tray.set_connection_state (gv::ui::ConnectionState::SignedOut);
      badge.set_signed_in (false);
      if (r.has_value ()) {
         tray.showMessage (QStringLiteral ("Signed out"), QStringLiteral ("Tokens cleared."),
                           QSystemTrayIcon::Information, 4000);
      } else {
         tray.showMessage (QStringLiteral ("Sign-out failed"),
                           QString::fromStdString (r.error ().message), QSystemTrayIcon::Warning,
                           6000);
      }
   });

   auto* auth_watch = new QTimer (&app);
   auth_watch->setInterval (10'000);
   QObject::connect (
      auth_watch, &QTimer::timeout, &app,
      [&, signed_state, signed_subject, settings_failure_notified] {
         session.reload ();
         const bool now = session.signed_in ();
         const auto principal = session.principal ();
         const auto subject = principal.value_or ("");
         const bool identity_changed = subject != *signed_subject;
         if (now == *signed_state && !identity_changed) return;

         settings_sync.stop ();
         collection.set_enabled (false);
         controller.set_authenticated (false);
         if (identity_changed || !now) {
            auto scoped = scope_settings (settings_repo, principal);
            if (!scoped.has_value ()) {
               tray.set_connection_state (gv::ui::ConnectionState::Degraded);
               badge.set_signed_in (false);
               gv::core::log::app.error ("settings external account scope failed: {}",
                                         scoped.error ().message);
               if (!*settings_failure_notified) {
                  *settings_failure_notified = true;
                  tray.showMessage (
                     QStringLiteral ("Settings unavailable"),
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
         publish_session_header ();
         tray.set_connection_state (now ? gv::ui::ConnectionState::Syncing
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

   gv::core::diagnostics::process_sample ();
   auto* perf_watch = new QTimer (&app);
   perf_watch->setInterval (60'000);
   QObject::connect (perf_watch, &QTimer::timeout, &app, [] {
      if (const auto sample = gv::core::diagnostics::process_sample (); !sample.empty ()) {
         gv::core::log::app.info ("perf {}", sample);
      }
   });
   perf_watch->start ();

   if (session.signed_in ()) {
      settings_sync.start ();
   } else if (opts.no_auto_login) {
      tray.showMessage (
         QStringLiteral ("GrimVault"),
         QStringLiteral ("Sign in via the tray to enable lookups. (Auto-login disabled.)"),
         QSystemTrayIcon::Information, 6000);
   } else {
      QTimer::singleShot (0, &app, do_sign_in);
   }

   QObject::connect (&tray, &gv::ui::TrayIcon::settings_requested, &app, [&active_env] {
      const QString url = QString::fromStdString (std::string { active_env.spa_base_url }) +
                          QStringLiteral ("/dashboard/grimvault");
      QDesktopServices::openUrl (QUrl (url));
   });
   QObject::connect (&tray, &gv::ui::TrayIcon::logs_requested, &app, [data_dir] {
      QDesktopServices::openUrl (
         QUrl::fromLocalFile (QString::fromStdString ((data_dir / "logs").string ())));
   });
   QObject::connect (&tray, &gv::ui::TrayIcon::check_updates_requested, &update_service,
                     &gv::update::UpdateService::check_now_with_ui);
   QObject::connect (&tray, &gv::ui::TrayIcon::quit_requested, &app, &QApplication::quit);

   QObject::connect (&update_service, &gv::update::UpdateService::shutdown_requested, &app,
                     &QApplication::quit, Qt::QueuedConnection);

   update_service.set_check_interval_seconds (3600);
   update_service.start ();

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

      gv::core::log::update.info (disabled_by_env ? "skipped (GRIMVAULT_DISABLE_UPDATES set)"
                                                  : "skipped (auto-updates disabled)");
   };

   QObject::connect (&settings_bridge, &gv::app::SettingsBridge::applied, &app, sync_updates);
   sync_updates ();

   const int rc = app.exec ();

   oauth->cancel_authorize ();
   for (auto* worker : oauth_workers) {
      worker->wait ();
      delete worker;
   }
   oauth_workers.clear ();

   settings_sync.stop ();
   controller.stop ();
   if (pipeline) pipeline->stop ();
   collection.stop ();
   tracker.reset ();
   hotkeys.reset ();
   update_service.stop ();

   gv::core::Logger::info ("GrimVault exiting with code {}", rc);
   gv::core::Logger::shutdown ();
   return rc;
}

// ---- CLI run loop ----
int run_cli_impl (int argc, char** argv, const std::vector<std::string>& cli_args)
{
   attach_console_impl ();

   QCoreApplication app (argc, argv);
   app.setApplicationName (QStringLiteral ("GrimVault"));
   app.setOrganizationName (QStringLiteral ("DDB"));
   app.setOrganizationDomain (QStringLiteral ("darkerdb.com"));

   gv::core::http::Global http;
   if (!http) return 1;

   auto data_dir = app_data_dir ();
   gv::core::Logger::init (data_dir / "logs", false);

   const int rc = gv::cli::run (cli_args);

   gv::core::Logger::shutdown ();
   return rc;
}

}

void attach_console ()
{
   attach_console_impl ();
}

void enable_dpi ()
{
   enable_dpi_impl ();
}

int run_gui (int argc, char** argv, GuiOptions options)
{
   return run_gui_impl (argc, argv, std::move (options));
}

int run_cli (int argc, char** argv, const std::vector<std::string>& args)
{
   return run_cli_impl (argc, argv, args);
}

}

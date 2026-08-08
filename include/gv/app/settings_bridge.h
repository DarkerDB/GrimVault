#pragma once

#include <gv/app/preferences.h>

#include <QObject>
#include <QString>

#include <memory>

namespace gv::app { class Controller; }
namespace gv::db  { class UserSettingsRepo; }
namespace gv::ui  { class OverlayWindow; }

namespace gv::app {

// Turns synced settings into live behaviour.
//
// SettingsSync owns the network side: it polls /v2/grimvault/settings and
// mirrors the response into UserSettingsRepo, emitting settings_changed per
// changed key. This is the other half — the part that makes a dashboard
// change actually do something, with no restart:
//
//    SettingsSync ──settings_changed(key,value)──► SettingsBridge
//                                                      │
//                            ┌─────────────────────────┼──────────────────┐
//                            ▼                         ▼                  ▼
//                     OverlayWindow            Controller          startup link /
//                  (layout + card options)   (mode, hotkeys)       auto-updates
//
// reload () seeds from the repo at startup so a signed-in relaunch applies
// the player's settings before the first poll comes back.
class SettingsBridge : public QObject
{
   Q_OBJECT

public:
   struct Dependencies {
      gv::db::UserSettingsRepo* repo       = nullptr;
      gv::ui::OverlayWindow*    overlay    = nullptr;
      gv::app::Controller*      controller = nullptr;

      // Absolute path to the running exe, for the HKCU\...\Run entry.
      std::string               exe_path;

      // --no-updates / GRIMVAULT_DISABLE_UPDATES: the dashboard's
      // auto-update toggle must not re-enable what the CLI turned off.
      bool                      updates_locked_off = false;
   };

   explicit SettingsBridge (Dependencies deps, QObject* parent = nullptr);
   ~SettingsBridge () override;

   // Fold every stored key and push the result. Call once after the repo is
   // open, before the first poll.
   void reload ();

   // One changed key from SettingsSync. Empty `value` = the key was erased.
   void apply (const QString& key, const QString& value);

   // Current folded state. Read for diagnostics and the CLI.
   const Preferences& preferences () const noexcept;

   // True when auto-updates are permitted: the dashboard toggle AND the
   // process-level lock.
   bool auto_updates_enabled () const noexcept;

signals:
   // Emitted after any change that altered behaviour, so the tray/badge can
   // re-read. Coalesced: one signal per apply () or reload ().
   void applied ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::app

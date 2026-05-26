#pragma once

#include <gv/core/result.h>
#include <gv/core/window_tracker.h>

#include <QObject>

#include <atomic>
#include <memory>
#include <string>

namespace gv::db  { class Database; class UserHotkeysRepo; class UserSettingsRepo; }
namespace gv::api { class DarkerDbClient; }
namespace gv::ocr { class Pipeline; struct RecognizedTooltip; }
namespace gv::core { class HotkeyManager; }
namespace gv::ui  { class OverlayWindow; }

namespace gv::app {

// Action identifiers used as hotkey ids and surfaced to the UI.
struct Actions {
   static constexpr const char* k_scan_now      = "scan_now";
   static constexpr const char* k_toggle_mode   = "toggle_mode";
   static constexpr const char* k_debug_toggle  = "debug_toggle";
   static constexpr const char* k_clear_overlay = "clear_overlay";
};

// Default accelerators applied when the user_hotkeys table has no rows.
struct DefaultAccelerators {
   static constexpr const char* scan_now      = "F5";
   static constexpr const char* toggle_mode   = "F6";
   static constexpr const char* debug_toggle  = "F7";
   static constexpr const char* clear_overlay = "F8";
};

enum class Mode { Auto, Manual };

// Owns the runtime wiring between the window tracker, capture pipeline,
// API client, overlay window, and hotkey manager. Lives on the Qt main
// thread; worker threads marshal back via queued invocations.
class Controller : public QObject
{
   Q_OBJECT

public:
   struct Dependencies {
      gv::db::Database*           db            = nullptr;
      gv::db::UserHotkeysRepo*    hotkeys_repo  = nullptr;
      gv::db::UserSettingsRepo*   settings_repo = nullptr;
      gv::api::DarkerDbClient*    api           = nullptr;
      gv::ocr::Pipeline*          pipeline      = nullptr;
      gv::core::HotkeyManager*    hotkeys       = nullptr;
      gv::ui::OverlayWindow*      overlay       = nullptr;
   };

   explicit Controller (Dependencies deps, QObject* parent = nullptr);
   ~Controller () override;

   // Bind all known actions using saved accelerators (falling back to defaults).
   // Returns the number of bindings that succeeded.
   int bind_hotkeys_from_repo ();

   // Replace the accelerator for `action`. Validates, persists, and re-binds.
   core::Result<void> rebind_hotkey (std::string action, std::string accelerator);

   // Returns the currently bound accelerator for an action (or default if unset).
   std::string accelerator_for (std::string_view action) const;

   Mode mode () const noexcept;

   // Action entry points. Each is safe to call from any thread; the heavy
   // lifting happens via QMetaObject::invokeMethod queued connection.
   void action_scan_now      ();
   void action_toggle_mode   ();
   void action_debug_toggle  ();
   void action_clear_overlay ();

   // Wired into WindowTracker. Called from the tracker's pump thread.
   void on_window_event (const core::WindowEvent& ev);

   // Wired into Pipeline. Called from the OCR worker thread.
   void on_tooltip (const ocr::RecognizedTooltip& rt);

signals:
   void modeChanged       (gv::app::Mode m);
   void overlayPresented  ();
   void overlayCleared    ();
   void hotkeysChanged    ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::app

Q_DECLARE_METATYPE (gv::app::Mode)

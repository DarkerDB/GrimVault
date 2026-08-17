#pragma once

#include <gv/app/mode.h>
#include <gv/capture/mode.h>
#include <gv/core/result.h>
#include <gv/core/window_tracker.h>

#include <QObject>
#include <QRect>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace gv::db  { class Database; class UserHotkeysRepo; class UserSettingsRepo; }
namespace gv::api { class DDBClient; }
namespace gv::ocr { class Pipeline; struct RecognizedTooltip; }
namespace gv::core { class HotkeyManager; }
namespace gv::ui  { class DebugOverlay; class OverlayWindow; }

namespace gv::app {

// Action identifiers used as hotkey ids and surfaced to the UI.
//
// Two of these are dashboard-bindable and map onto the hotkeys:* settings
// group: `hotkeys:force_refresh` drives k_scan_now and
// `hotkeys:toggle_overlay` drives k_toggle_overlay. The rest keep their
// local defaults — toggle_mode and clear_overlay are power-user affordances
// and debug_toggle only does anything under --debug=highlight:*, so none of
// them earns a row in the dashboard.
struct Actions {
   static constexpr const char* k_scan_now       = "scan_now";
   static constexpr const char* k_toggle_mode    = "toggle_mode";
   static constexpr const char* k_debug_toggle   = "debug_toggle";
   static constexpr const char* k_clear_overlay  = "clear_overlay";
   static constexpr const char* k_toggle_overlay = "toggle_overlay";
   static constexpr const char* k_open_in_browser = "open_in_browser";
};

// Default accelerators applied when the user_hotkeys table has no rows.
struct DefaultAccelerators {
   static constexpr const char* scan_now       = "F5";
   static constexpr const char* toggle_mode    = "F6";
   static constexpr const char* debug_toggle   = "F7";
   static constexpr const char* clear_overlay  = "F8";
   static constexpr const char* toggle_overlay  = "Ctrl+Shift+G";
   static constexpr const char* open_in_browser = "Ctrl+Shift+D";
};

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
      gv::api::DDBClient*         api           = nullptr;
      gv::ocr::Pipeline*          pipeline      = nullptr;
      gv::core::HotkeyManager*    hotkeys       = nullptr;
      gv::ui::OverlayWindow*      overlay       = nullptr;
      gv::ui::DebugOverlay*       debug         = nullptr;
      bool                        highlight_game = false;
      bool                        highlight_objects = false;
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

   // Authentication is a runtime boundary. Signed-out clients perform no
   // capture, OCR, analysis, persistence, or presentation work.
   void set_authenticated (bool authenticated, std::string principal = {});

   // Cancel pending network work and join owned workers. Idempotent.
   void stop ();

   // The dashboard's overlay:mode. Remembered separately from the live mode
   // so toggle_overlay can flip to Disabled and back to whatever the player
   // actually configured, rather than always landing on Auto.
   void set_configured_mode (Mode m);

   // Apply the dashboard's maximum capture/readback rate live.
   void set_capture_fps (int fps);

   // Apply the dashboard's capture-backend policy live.
   void set_capture_mode (capture::CaptureMode mode);

   void set_performance_mode (bool on);

   void set_language (std::string selection);

   // Enabled dashboard widgets sent as a computation hint. The API still
   // intersects these with the authenticated plan before doing less work.
   void set_enabled_widgets (std::vector<std::string> widgets);

   // Action entry points. Each is safe to call from any thread; the heavy
   // lifting happens via QMetaObject::invokeMethod queued connection.
   void action_scan_now       ();
   void action_toggle_mode    ();
   void action_debug_toggle   ();
   void action_clear_overlay  ();
   void action_toggle_overlay ();

   // Open the last analysed item's page on the SPA. The card truncates long
   // lists (recipes especially) and advertises this key as the way to see
   // the rest, so it has to work off whatever was last hovered.
   void action_open_in_browser ();

   // SPA origin for that action, e.g. https://darkerdb.com.
   void set_browse_base (std::string url);

   // Wired into WindowTracker. Called from the tracker's pump thread.
   void on_window_event (const core::WindowEvent& ev);

   // Wired into Pipeline. Called from the OCR worker thread.
   void on_tooltip (const ocr::RecognizedTooltip& rt);

signals:
   void modeChanged       (gv::app::Mode m);
   void overlayPresented  ();
   void overlayCleared    ();
   void hotkeysChanged    ();

   // Game window moved / resized / focus-flipped. `active` == visible AND
   // focused; consumers (status badge) hide themselves when it drops.
   void gameWindowChanged (QRect bounds, bool active);

   // A tooltip made it through detection + OCR (whether or not the lookup
   // then succeeded) — UI feedback that the pipeline is alive.
   void scanActivity      ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::app

Q_DECLARE_METATYPE (gv::app::Mode)

Both additions kept (no conflict in intent): `set_performance_mode` and `set_language`. Implementation for each still needs to exist in `controller.cpp`, worth checking.

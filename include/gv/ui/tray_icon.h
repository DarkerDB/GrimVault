#pragma once

#include <gv/ui/tray_menu.h>

#include <QSystemTrayIcon>

#include <memory>

namespace gv::ui {

// System-tray icon + branded popup menu. Right-click pops a TrayMenu
// (frameless, palette-driven) instead of the OS menu. High-level intents
// flow out as Qt signals; main () wires them to the auth session,
// overlay daemon, and update service.
class TrayIcon : public QSystemTrayIcon
{
   Q_OBJECT

public:
   explicit TrayIcon (QObject* parent = nullptr);
   ~TrayIcon () override;

   // Toggles the auth label between "Sign In" and "Log Out", and recolors
   // the header status dot green (signed in) / red (signed out).
   void set_signed_in (bool signed_in);
   void set_connection_state (ConnectionState state);

signals:
   void sign_in_requested        ();
   void sign_out_requested       ();
   void settings_requested       ();
   void logs_requested           ();
   void check_updates_requested  ();
   void quit_requested           ();

private:
   std::unique_ptr<TrayMenu> menu_;
};

} // namespace gv::ui

#pragma once

#include <QSystemTrayIcon>

class QAction;

namespace gv::ui {

// System-tray icon. Emits high-level intents; main () wires them to the
// auth session / overlay daemon / update service. Right-click → menu.
// The "Sign In / Sign Out" item is dynamic — call set_signed_in (bool)
// when session state changes.
class TrayIcon : public QSystemTrayIcon
{
   Q_OBJECT

public:
   explicit TrayIcon (QObject* parent = nullptr);

   // Swap the Sign In / Sign Out item label and emitted signal.
   void set_signed_in (bool signed_in);

signals:
   void sign_in_requested  ();
   void sign_out_requested ();
   void open_dashboard_requested ();
   void logs_requested    ();
   void check_updates_requested ();
   void quit_requested    ();

private:
   QAction* auth_action_ = nullptr;
   bool     signed_in_   = false;

   void update_auth_label ();
};

} // namespace gv::ui

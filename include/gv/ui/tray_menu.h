#pragma once

#include <QString>
#include <QWidget>

#include <memory>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace gv::ui {

enum class ConnectionState {
   SignedOut,
   Syncing,
   Ready,
   Degraded,
};

// Frameless popup styled with the GrimVault palette (see qml/Palette.qml).
// Rounded panel, soft shadow, palette-driven hover, single header line
// ("GrimVault (Env) v0.0.1") with a status dot that's green when the
// user is signed in and red when they're not.
//
// Owned and shown by TrayIcon. set_signed_in () is the only state
// driver — the header label is static (env + version are baked at
// startup); the dot recolors when sign-in state flips.
class TrayMenu : public QWidget
{
   Q_OBJECT

public:
   explicit TrayMenu (QWidget* parent = nullptr);

   // Flip the auth label between "Sign In" (signed out) and "Log Out"
   // (signed in), and recolor the status dot (green/red).
   void set_signed_in (bool yes);
   void set_connection_state (ConnectionState state);

   // Pop the menu so its bottom-right corner sits at `global_pos`
   // (cursor position). Clamps to the screen the cursor is on.
   void popup_at (const QPoint& global_pos);

signals:
   void sign_in_requested        ();
   void sign_out_requested       ();
   void settings_requested       ();
   void logs_requested           ();
   void check_updates_requested  ();
   void quit_requested           ();

protected:
   void paintEvent (QPaintEvent* ev) override;
   void hideEvent  (QHideEvent*  ev) override;

private:
   QPushButton* auth_btn_   = nullptr;
   QLabel*      status_label_ = nullptr;
   QWidget*     status_dot_ = nullptr;
   bool         signed_in_  = false;
   ConnectionState state_   = ConnectionState::SignedOut;

   QPushButton* add_item       (QVBoxLayout* body, const QString& label);
   QWidget*     make_separator () const;
   void         refresh_dot    ();
   void         refresh_auth   ();
   void         refresh_status ();
};

} // namespace gv::ui

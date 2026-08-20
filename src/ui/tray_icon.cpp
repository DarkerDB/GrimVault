#include <gv/ui/tray_icon.h>

#include <gv/core/version.h>

#include <QCursor>
#include <QIcon>

namespace gv::ui {

TrayIcon::TrayIcon (QObject* parent)
   : QSystemTrayIcon (parent)
{
   setIcon    (QIcon (QStringLiteral (":/assets/images/Icon-324x356.png")));
   setToolTip (QStringLiteral ("GrimVault %1").arg (
      QString::fromLatin1 (gv::core::version::string)));

   menu_ = std::make_unique<TrayMenu> ();

   // Forward TrayMenu signals as TrayIcon signals so existing main () wiring
   // doesn't have to know about TrayMenu at all.
   connect (menu_.get (), &TrayMenu::sign_in_requested,        this, &TrayIcon::sign_in_requested);
   connect (menu_.get (), &TrayMenu::sign_out_requested,       this, &TrayIcon::sign_out_requested);
   connect (menu_.get (), &TrayMenu::settings_requested,       this, &TrayIcon::settings_requested);
   connect (menu_.get (), &TrayMenu::logs_requested,           this, &TrayIcon::logs_requested);
   connect (menu_.get (), &TrayMenu::check_updates_requested,  this, &TrayIcon::check_updates_requested);
   connect (menu_.get (), &TrayMenu::renderer_requested,       this, &TrayIcon::renderer_requested);
   connect (menu_.get (), &TrayMenu::quit_requested,           this, &TrayIcon::quit_requested);

   // No setContextMenu (...) — leaving it unset means right-click on
   // Windows fires activated (Context) instead of showing the native
   // QMenu, which is exactly what we want.
   connect (this, &QSystemTrayIcon::activated,
      this, [this] (QSystemTrayIcon::ActivationReason reason) {
         if (reason != QSystemTrayIcon::Context) return;
         // Toggle — right-clicking the icon again while open hides it,
         // matches Windows menu reflexes.
         if (menu_->isVisible ()) menu_->hide ();
         else                     menu_->popup_at (QCursor::pos ());
      });

   QSystemTrayIcon::show ();
}

TrayIcon::~TrayIcon () = default;

void TrayIcon::set_signed_in (bool signed_in)
{
   if (menu_) menu_->set_signed_in (signed_in);
}

void TrayIcon::set_connection_state (ConnectionState state)
{
   if (menu_) menu_->set_connection_state (state);
}

void TrayIcon::set_renderer (const QString& renderer)
{
   if (menu_) menu_->set_renderer (renderer);
}

} // namespace gv::ui

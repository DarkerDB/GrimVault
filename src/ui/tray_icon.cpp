#include <gv/ui/tray_icon.h>
#include <gv/core/version.h>

#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QMenu>

namespace gv::ui {

TrayIcon::TrayIcon (QObject* parent)
   : QSystemTrayIcon (parent)
{
   setIcon    (QIcon (QStringLiteral (":/assets/images/Icon-324x356.png")));
   setToolTip (QStringLiteral ("GrimVault %1").arg (
      QString::fromLatin1 (gv::core::version::string)));

   auto* menu = new QMenu ();

   auth_action_ = menu->addAction (QStringLiteral ("Sign in to DarkerDB"));
   connect (auth_action_, &QAction::triggered, this, [this] {
      if (signed_in_) emit sign_out_requested ();
      else            emit sign_in_requested  ();
   });

   auto* dash_action = menu->addAction (QStringLiteral ("Open Dashboard"));
   connect (dash_action, &QAction::triggered, this, &TrayIcon::open_dashboard_requested);

   menu->addSeparator ();

   auto* logs_action = menu->addAction (QStringLiteral ("Open logs folder"));
   connect (logs_action, &QAction::triggered, this, &TrayIcon::logs_requested);

   auto* check_action = menu->addAction (QStringLiteral ("Check for updates"));
   connect (check_action, &QAction::triggered, this, &TrayIcon::check_updates_requested);

   auto* version = menu->addAction (QStringLiteral ("Version %1").arg (
      QString::fromLatin1 (gv::core::version::string)));
   version->setEnabled (false);

   menu->addSeparator ();

   auto* quit = menu->addAction (QStringLiteral ("Quit"));
   connect (quit, &QAction::triggered, this, &TrayIcon::quit_requested);

   setContextMenu (menu);

   QSystemTrayIcon::show ();
}

void TrayIcon::set_signed_in (bool signed_in)
{
   signed_in_ = signed_in;
   update_auth_label ();
}

void TrayIcon::update_auth_label ()
{
   if (!auth_action_) return;
   auth_action_->setText (signed_in_
      ? QStringLiteral ("Sign out")
      : QStringLiteral ("Sign in to DarkerDB"));
}

} // namespace gv::ui

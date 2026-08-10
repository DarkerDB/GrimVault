#pragma once

#include <QObject>
#include <QTimer>

namespace gv::update {

// Thin Qt wrapper around WinSparkle. The appcast URL is baked into the
// binary at build time via the GRIMVAULT_APPCAST_URL CMake variable
// (defaults to https://katforge-releases.s3.us-west-2.amazonaws.com/grimvault/appcast.xml).
//
// Lifecycle: construct once at app start, then call start() after the main
// window is shown. WinSparkle's scheduler stays disabled: the dashboard
// setting controls this service's timer, avoiding WinSparkle's separate
// first-run consent and keeping live setting changes authoritative.
class UpdateService : public QObject
{
   Q_OBJECT

public:
   explicit UpdateService (QObject* parent = nullptr);
   ~UpdateService () override;

   void start ();
   void stop  () noexcept;

   void check_now_with_ui ();
   void check_now_silent  ();

   void set_check_interval_seconds (int seconds);
   void set_automatic_checks_enabled (bool enabled);

signals:
   void update_available ();
   void update_dismissed ();
   void update_error     ();

   // WinSparkle is about to run the downloaded installer and needs the app
   // gone so it can overwrite the binaries. Emitted from a WinSparkle worker
   // thread; connect queued and exit the application promptly.
   void shutdown_requested ();

private:
   QTimer check_timer_;
   bool   initialized_ = false;
};

} // namespace gv::update

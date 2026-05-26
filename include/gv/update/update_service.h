#pragma once

#include <QObject>

namespace gv::update {

// Thin Qt wrapper around WinSparkle. The appcast URL is baked into the
// binary at build time via the GRIMVAULT_APPCAST_URL CMake variable
// (defaults to https://katforge-releases.s3.us-west-2.amazonaws.com/grimvault/appcast.xml).
//
// Lifecycle: construct once at app start, call start() after the main
// window is shown. WinSparkle owns its own check thread; stop() must run
// before app exit.
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

signals:
   void update_available ();
   void update_dismissed ();
   void update_error     ();
};

} // namespace gv::update

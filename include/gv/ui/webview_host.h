#pragma once

#include <gv/core/result.h>

#include <QRect>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace gv::ui {

// Composition-hosted WebView2 in a transparent, click-through, topmost
// Win32 popup window. Web content never receives input (composition mode
// only forwards what we send — nothing) and the wndproc answers
// WM_NCHITTEST with HTTRANSPARENT, so clicks land in the game underneath.
//
// Rendering goes through DirectComposition (WS_EX_NOREDIRECTIONBITMAP), so
// alpha from the page composes cleanly over the game with no redirection-
// surface flicker. All geometry is physical pixels: bounds mode is
// COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS and the caller drives
// rasterization scale per monitor.
//
// Native <-> page messages are single-line JSON over PostWebMessageAsJson /
// window.chrome.webview. Creation is async; post_json() before ready() is
// dropped with a log line, so callers gate on on_ready.
//
// Must be created and used on the Qt GUI thread (STA; Qt's Windows event
// dispatcher pumps the messages WebView2 needs).
class WebviewHost
{
public:
   struct Config
   {
      // Folder mapped to https://grimvault.assets/ (virtual host); the
      // initial navigation loads <web_dir>/augment.html from it.
      std::filesystem::path web_dir;

      // WebView2 user-data folder (%APPDATA%\GrimVault\webview2).
      std::filesystem::path user_data_dir;
   };

   struct Callbacks
   {
      std::function<void ()>                   on_ready;
      std::function<void (std::string_view)>   on_message;

      // Browser process died or the runtime updated under us. The host has
      // already torn down; owner decides whether to recreate or fall back.
      std::function<void ()>                   on_process_failed;
   };

   // Fails fast when the Evergreen runtime is absent (returns the installed
   // runtime version string on success).
   static core::Result<std::string> runtime_version ();

   static core::Result<std::unique_ptr<WebviewHost>> create (Config config,
                                                             Callbacks callbacks);
   ~WebviewHost ();

   WebviewHost (const WebviewHost&)            = delete;
   WebviewHost& operator= (const WebviewHost&) = delete;

   bool ready () const;

   void post_json (const std::string& json);

   // Position + size the window in physical screen pixels and update the
   // WebView2 rasterization scale for the target monitor.
   void place (const QRect& physical, double scale);

   void show ();
   void hide ();

private:
   WebviewHost ();

   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ui

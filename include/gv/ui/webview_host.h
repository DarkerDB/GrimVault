#pragma once

#include <gv/core/result.h>

#include <QSize>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace gv::ui {

class WebviewHost
{
public:
   struct Config
   {
      // Folder mapped to https://grimvault.assets/ (virtual host); the
      // initial navigation loads <web_dir>/augment.html from it.
      std::filesystem::path web_dir;

      // WebView2 user-data folder under the active LocalAppData directory.
      std::filesystem::path user_data_dir;

      bool software_rendering = false;
   };

   struct Callbacks
   {
      std::function<void ()>                   on_ready;
      std::function<void (std::string_view)>   on_message;

      std::function<void (std::string)>        on_failed;
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

   void resize (const QSize& physical, double scale);

   // Capture the current WebView viewport as PNG bytes. Callback runs on the
   // GUI/COM apartment thread. An empty vector indicates capture failure.
   void capture_png (std::function<void (std::vector<std::uint8_t>)> callback);

private:
   WebviewHost ();

   struct Impl;
   // Shared with the asynchronous WebView2 creation callbacks. Public
   // methods release their owner in the destructor; an in-progress callback
   // keeps Impl alive only until that callback completes.
   std::shared_ptr<Impl> impl_;
};

} // namespace gv::ui

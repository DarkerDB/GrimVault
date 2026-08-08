#pragma once

#include <QObject>
#include <QRect>

#include <filesystem>
#include <memory>
#include <string>

namespace gv::api { struct TooltipLookup; }

namespace gv::ui {

// The overlay surface that draws the Augment beside the in-game tooltip.
// Renderer is WebView2 + ddb-tooltips (AugmentView) by default; the legacy
// QML card remains as fallback when the WebView2 runtime is missing or its
// browser process dies (`overlay:renderer` = webview | qml).
//
// present()/clear() are the whole contract; Controller and main() are
// renderer-agnostic. Both rects are physical (Win32) screen pixels: game
// is the game window, anchor the detected tooltip box.
class OverlayWindow : public QObject
{
   Q_OBJECT

public:
   struct Config
   {
      // Directory holding augment.html + the vendored ddb-tooltips dist.
      std::filesystem::path web_dir;

      // WebView2 user-data folder (under %APPDATA%\GrimVault).
      std::filesystem::path user_data_dir;

      // "webview" (default) or "qml".
      std::string renderer = "webview";
   };

   explicit OverlayWindow (Config config, QObject* parent = nullptr);
   ~OverlayWindow () override;

   void present (const gv::api::TooltipLookup& lookup,
                 const QRect& game, const QRect& anchor);
   void clear ();

private:
   void fall_back_to_qml ();

   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ui

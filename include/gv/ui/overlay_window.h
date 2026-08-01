#pragma once

#include <gv/ui/layout.h>

#include <QObject>
#include <QRect>

#include <filesystem>
#include <memory>
#include <string>

namespace gv::api { struct TooltipLookup; }

namespace gv::ui {

namespace augment { struct Options; }

// The overlay surface that draws the Augment beside the in-game tooltip.
// Renderer is the DDB SDK in a permanently hidden WebView2, captured to a
// bitmap and presented by a disabled native window. The QML card remains as
// fallback (`overlay:renderer` = webview | qml).
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

      // "webview" (hidden snapshot renderer, default) or "qml".
      std::string renderer = "webview";
   };

   explicit OverlayWindow (Config config, QObject* parent = nullptr);
   ~OverlayWindow () override;

   void present (const gv::api::TooltipLookup& lookup,
                 const QRect& game, const QRect& anchor, bool animate = true);
   void clear ();

   // Skeleton card, shown the moment a region is anchored so the hover feels
   // answered before the analysis actually is. No-op on the QML fallback.
   void present_loading ();

   // Live settings. Both are cheap and idempotent — SettingsBridge calls
   // them whenever the dashboard changes, including mid-hover.
   void set_layout  (const Layout& layout);
   void set_options (const augment::Options& options);

   // Anchoring passthrough (WebView2 renderer only; QML is lookup-driven).
   void anchor_shown (const QRect& game, const QPoint& offset, const QSize& tip,
                      bool pinned_x, bool pinned_y, const QPoint& pin);
   void anchor_lost (bool immediate);

private:
   void fall_back_to_qml ();

   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ui
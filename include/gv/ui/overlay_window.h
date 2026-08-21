#pragma once

#include <gv/ui/layout.h>

#include <QObject>
#include <QRect>
#include <QString>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace gv::api { struct TooltipLookup; }

namespace gv::ui {

namespace augment { struct Options; }

// The overlay surface that draws the Augment beside the in-game tooltip.
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

      // WebView2 user-data folder under the active LocalAppData directory.
      std::filesystem::path user_data_dir;
   };

   explicit OverlayWindow (Config config, QObject* parent = nullptr);
   ~OverlayWindow () override;

   void present (const gv::api::TooltipLookup& lookup,
                 const QRect& game, const QRect& anchor, bool animate = true);
   void clear ();
   bool set_active (bool active);

   // Live settings. Both are cheap and idempotent — SettingsBridge calls
   // them whenever the dashboard changes, including mid-hover.
   void set_layout  (const Layout& layout);
   void set_options (const augment::Options& options);

   void anchor_shown (const QRect& game, const QPoint& offset, const QSize& tip,
                      bool pinned_x, bool pinned_y, const QPoint& pin);
   void anchor_lost (bool immediate);

signals:
   void renderer_failed (const QString& reason);

private:
   void start_webview ();
   void recover_webview (std::uint64_t generation, std::string reason);
   void fail_webview (std::string reason);

   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ui

#pragma once

#include <gv/core/result.h>
#include <gv/ui/webview_host.h>

#include <QRect>

#include <filesystem>
#include <functional>
#include <memory>

namespace gv::api { struct TooltipLookup; }

namespace gv::ui {

// The Augment: GrimVault's overlay card, rendered by the real ddb-tooltips
// library inside a WebviewHost. present() posts a render payload; the page
// renders and reports its CSS-pixel size back; only the matching-seq size
// message places and shows the window (no wrong-size flash).
//
//    present(lookup)                          {type:'size', seq, w, h}
//       │  {type:'render', seq, entity,...}      │
//       ▼                                        ▼
//    WebviewHost ──► DDB.tooltips.render ──► ResizeObserver ──► place+show
//
// Placement mirrors the QML overlay: physical pixels, flip to the anchor's
// left when the right side would overflow, clamped inside the game window.
class AugmentView
{
public:
   struct Config
   {
      std::filesystem::path web_dir;
      std::filesystem::path user_data_dir;
   };

   // on_failed: WebView2 unavailable or died and could not serve renders;
   // the owner switches to the QML fallback renderer.
   static core::Result<std::unique_ptr<AugmentView>> create (Config config,
                                                             std::function<void ()> on_failed);
   ~AugmentView ();

   void present (const gv::api::TooltipLookup& lookup,
                 const QRect& game, const QRect& anchor, bool animate = true);
   void clear ();

   // Anchoring (docs/architecture/anchoring.md §7): show the card beside
   // the anchored tooltip and follow it at presenter rate; anchor-lost
   // hides after a short grace so transient losses never blink the card.
   void anchor_shown (const QRect& game, const QPoint& offset, const QSize& tip,
                      bool pinned_x, bool pinned_y, const QPoint& pin);

   // immediate = a cursor-jump reset: hide now, no grace.
   void anchor_lost (bool immediate);

private:
   AugmentView ();

   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ui

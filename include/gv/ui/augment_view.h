#pragma once

#include <gv/core/result.h>
#include <gv/ui/augment_payload.h>
#include <gv/ui/overlay_window.h>
#include <gv/ui/webview_host.h>

#include <QRect>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace gv::api {
struct TooltipLookup;
}

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
class AugmentView
{
  public:
   struct Config {
      std::filesystem::path web_dir;
      std::filesystem::path user_data_dir;
      bool software_rendering = false;
   };

   struct Callbacks {
      std::function<void ()> on_ready;
      std::function<void (std::string)> on_failed;

      // The card's window in physical screen pixels whenever it is shown or
      // moves; an empty rect once it is hidden.
      std::function<void (QRect)> on_card_area;
   };

   static core::Result<std::unique_ptr<AugmentView>> create (Config config, Callbacks callbacks);
   ~AugmentView ();

   void present (const gv::api::TooltipLookup& lookup, const QRect& game, const QRect& anchor,
                 bool animate = true);

   void clear ();

   // Follow the current tooltip at presenter rate. A lost or replaced
   // anchor invalidates its card immediately, including queued renders.
   void anchor_shown (const QRect& game, const QPoint& offset, const QSize& tip, bool pinned_x,
                      bool pinned_y, const QPoint& pin);

   // Example: loss after presenting Bandage leaves no visible or queued card.
   void anchor_lost ();

   // Live dashboard settings. set_layout takes effect on the next placement
   // (opacity repaints immediately); set_options on the next render.
   void set_layout (const Layout& layout);
   void set_options (const augment::Options& options);

  private:
   AugmentView ();

   struct Impl;
   std::unique_ptr<Impl> impl_;
};

}  // namespace gv::ui

#include <gv/ui/augment_view.h>
#include <gv/api/darkerdb_client.h>
#include <gv/core/logger.h>
#include <gv/ui/augment_payload.h>
#include <gv/ui/screen.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>

namespace gv::ui {

using nlohmann::json;

struct AugmentView::Impl
{
   std::unique_ptr<WebviewHost> host;
   std::function<void ()>       on_failed;

   std::uint64_t seq         = 0;
   std::uint64_t pending_seq = 0;
   QRect         game;
   QRect         anchor;
   bool          warm        = false;

   void on_message (std::string_view text);
   void place_and_show (int css_w, int css_h);
   void prewarm ();
};

AugmentView::AugmentView () : impl_ (std::make_unique<Impl> ()) {}

AugmentView::~AugmentView () = default;

core::Result<std::unique_ptr<AugmentView>> AugmentView::create (Config config,
                                                                std::function<void ()> on_failed)
{
   auto view = std::unique_ptr<AugmentView> { new AugmentView () };
   auto* impl = view->impl_.get ();
   impl->on_failed = std::move (on_failed);

   auto host = WebviewHost::create (
      WebviewHost::Config {
         .web_dir       = std::move (config.web_dir),
         .user_data_dir = std::move (config.user_data_dir),
      },
      WebviewHost::Callbacks {
         .on_ready          = [impl] { impl->prewarm (); },
         .on_message        = [impl] (std::string_view text) { impl->on_message (text); },
         .on_process_failed = [impl] { if (impl->on_failed) impl->on_failed (); },
      });

   if (!host.has_value ()) {
      return core::fail (host.error ());
   }

   impl->host = std::move (*host);
   return view;
}

// First render pays WebView2's cold start (font load, style, raster). Run
// it hidden at startup so the first real present is steady-state.
void AugmentView::Impl::prewarm ()
{
   if (warm) return;

   warm = true;
   host->post_json (json {
      { "type", "render" },
      { "seq",  0 },
      { "entity", json {
         { "name",     "<GrimVault>" },
         { "rarity",   "common" },
         { "sections", json::array ({ { { "kind", "text" }, { "title", "warmup" } } }) },
      } },
      { "params", { { "kind", "augment" }, { "compact", true } } },
   }.dump ());
}

void AugmentView::present (const gv::api::TooltipLookup& lookup,
                           const QRect& game, const QRect& anchor)
{
   auto* impl = impl_.get ();

   if (!impl->host || !impl->host->ready ()) {
      core::Logger::debug ("augment: present before webview ready dropped");
      return;
   }

   impl->pending_seq = ++impl->seq;
   impl->game        = game;
   impl->anchor      = anchor;

   impl->host->post_json (augment::render_message (lookup, impl->pending_seq).dump ());

   core::Logger::info ("augment: render '{}' seq={}", lookup.canonical_name, impl->seq);
}

void AugmentView::clear ()
{
   impl_->pending_seq = 0;
   if (impl_->host) {
      impl_->host->hide ();
      impl_->host->post_json (json { { "type", "clear" } }.dump ());
   }
}

void AugmentView::Impl::on_message (std::string_view text)
{
   json msg = json::parse (text, nullptr, /*allow_exceptions=*/ false);
   if (msg.is_discarded ()) return;

   if (msg.value ("type", "") == "error") {
      core::Logger::error ("augment: page render error: {}", msg.value ("message", ""));
      return;
   }

   if (msg.value ("type", "") != "size") return;

   const auto msg_seq = msg.value ("seq", std::uint64_t { 0 });
   if (msg_seq == 0 || msg_seq != pending_seq) return;

   place_and_show (msg.value ("w", 0), msg.value ("h", 0));
}

void AugmentView::Impl::place_and_show (int css_w, int css_h)
{
   if (css_w <= 0 || css_h <= 0) return;

   const qreal s   = screen::scale_at (game.center ());
   const int   gap = static_cast<int> (12 * s);
   const int   w   = static_cast<int> (css_w * s);
   const int   h   = static_cast<int> (css_h * s);

   int x = anchor.x () + anchor.width () + gap;
   if (x + w > game.x () + game.width ()) {
      x = anchor.x () - gap - w;
   }
   int y = anchor.y ();

   x = std::max (game.x (), std::min (x, game.x () + game.width ()  - w));
   y = std::max (game.y (), std::min (y, game.y () + game.height () - h));

   host->place (QRect { x, y, w, h }, s);
   host->show ();

   core::Logger::info ("augment: shown at {},{} {}x{} physical (scale {})", x, y, w, h, s);
}

} // namespace gv::ui

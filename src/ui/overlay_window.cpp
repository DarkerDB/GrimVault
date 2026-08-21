#include <gv/ui/overlay_window.h>
#include <gv/api/darkerdb_client.h>
#include <gv/core/logger.h>
#include <gv/ui/augment_view.h>

#include <QMetaObject>
#include <QTemporaryDir>
#include <QTimer>

#include <optional>
#include <utility>
#include <vector>

namespace gv::ui {

struct OverlayWindow::Impl
{
   struct LastPresentation {
      gv::api::TooltipLookup lookup;
      QRect game;
      QRect anchor;
   };

   Config config;
   Layout layout;
   augment::Options options;
   std::vector<std::unique_ptr<QTemporaryDir>> profiles;
   std::unique_ptr<AugmentView> augment;
   std::optional<LastPresentation> last;
   QTimer startup;
   std::uint64_t generation = 0;
   int attempt = 0;
   bool active = true;
   bool failure_reported = false;
};

OverlayWindow::OverlayWindow (Config config, QObject* parent)
   : QObject (parent), impl_ (std::make_unique<Impl> ())
{
   impl_->config = std::move (config);
   impl_->startup.setSingleShot (true);
   impl_->startup.setInterval (10000);
   QObject::connect (&impl_->startup, &QTimer::timeout, this, [this] {
      recover_webview (impl_->generation, "startup timeout");
   });
   start_webview ();
}

OverlayWindow::~OverlayWindow () = default;

void OverlayWindow::start_webview ()
{
   const auto generation = ++impl_->generation;
   const bool software = impl_->attempt == 2;
   auto profile = impl_->config.user_data_dir;

   if (impl_->attempt > 0) {
      const auto pattern = impl_->config.user_data_dir.parent_path ()
         / "webview2-recovery-XXXXXX";
      auto temporary = std::make_unique<QTemporaryDir> (
         QString::fromStdWString (pattern.wstring ()));
      if (!temporary->isValid ()) {
         QTimer::singleShot (0, this, [this, generation] {
            recover_webview (generation, "temporary profile unavailable");
         });
         return;
      }
      profile = temporary->path ().toStdWString ();
      impl_->profiles.push_back (std::move (temporary));
   }

   core::Logger::info ("overlay: WebView2 attempt={} profile={} rendering={}",
      impl_->attempt + 1, impl_->attempt == 0 ? "persistent" : "temporary",
      software ? "software" : "hardware");

   auto augment = AugmentView::create (
      AugmentView::Config {
         .web_dir = impl_->config.web_dir,
         .user_data_dir = std::move (profile),
         .software_rendering = software,
      },
      AugmentView::Callbacks {
         .on_ready = [this, generation] {
            if (generation != impl_->generation) return;
            impl_->startup.stop ();
            core::Logger::info ("overlay: WebView2 ready attempt={}", impl_->attempt + 1);
         },
         .on_failed = [this, generation] (std::string reason) {
            QMetaObject::invokeMethod (this,
               [this, generation, reason = std::move (reason)] {
                  recover_webview (generation, reason);
               }, Qt::QueuedConnection);
         },
      });

   if (!augment.has_value ()) {
      const auto reason = augment.error ().message;
      QTimer::singleShot (0, this, [this, generation, reason] {
         recover_webview (generation, reason);
      });
      return;
   }

   impl_->augment = std::move (*augment);
   impl_->augment->set_layout (impl_->layout);
   impl_->augment->set_options (impl_->options);
   impl_->startup.start ();

   if (impl_->active && impl_->last) {
      impl_->augment->present (
         impl_->last->lookup, impl_->last->game, impl_->last->anchor, false);
   }
}

void OverlayWindow::recover_webview (std::uint64_t generation, std::string reason)
{
   if (generation != impl_->generation) return;

   impl_->startup.stop ();
   impl_->augment.reset ();
   core::Logger::warn ("overlay: WebView2 attempt={} failed: {}",
      impl_->attempt + 1, reason);

   if (++impl_->attempt < 3) {
      start_webview ();
      return;
   }

   fail_webview (std::move (reason));
}

void OverlayWindow::fail_webview (std::string reason)
{
   core::Logger::error ("overlay: WebView2 recovery exhausted: {}", reason);
   if (impl_->failure_reported) return;
   impl_->failure_reported = true;
   emit renderer_failed (QString::fromStdString (reason));
}

void OverlayWindow::present (const gv::api::TooltipLookup& lookup,
                             const QRect& game, const QRect& anchor, bool animate)
{
   impl_->last = Impl::LastPresentation { lookup, game, anchor };
   if (impl_->active && impl_->augment) {
      impl_->augment->present (lookup, game, anchor, animate);
   }
}

void OverlayWindow::clear ()
{
   impl_->last.reset ();
   if (impl_->augment) impl_->augment->clear ();
}

bool OverlayWindow::set_active (bool active)
{
   if (impl_->active == active) return false;
   impl_->active = active;

   if (!active) {
      if (impl_->augment) impl_->augment->clear ();
      return false;
   }

   if (!impl_->last || !impl_->augment) return false;
   const auto& last = *impl_->last;
   impl_->augment->present (last.lookup, last.game, last.anchor, false);
   return true;
}

void OverlayWindow::set_layout (const Layout& layout)
{
   impl_->layout = layout;
   if (impl_->augment) impl_->augment->set_layout (layout);
}

void OverlayWindow::set_options (const augment::Options& options)
{
   impl_->options = options;
   if (impl_->augment) impl_->augment->set_options (options);
}

void OverlayWindow::anchor_shown (const QRect& game, const QPoint& offset,
                                  const QSize& tip, bool pinned_x, bool pinned_y,
                                  const QPoint& pin)
{
   if (impl_->augment) {
      impl_->augment->anchor_shown (game, offset, tip, pinned_x, pinned_y, pin);
   }
}

void OverlayWindow::anchor_lost (bool immediate)
{
   if (impl_->augment) impl_->augment->anchor_lost (immediate);
   if (immediate) impl_->last.reset ();
}

}

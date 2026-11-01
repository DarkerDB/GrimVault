#include <gv/api/darkerdb_client.h>
#include <gv/ui/overlay_window.h>

#include <QEventLoop>
#include <QTemporaryDir>
#include <QTimer>

#include <gtest/gtest.h>

#include <filesystem>
#include <functional>

namespace {

// Pump real WebView2 callbacks until a predicate succeeds or time expires.
// Example: an already visible card returns true without waiting.
bool wait_until (const std::function<bool ()>& predicate, int milliseconds)
{
   if (predicate ()) {
      return true;
   }

   QEventLoop loop;
   QTimer poll;
   QTimer deadline;
   deadline.setSingleShot (true);
   QObject::connect (&poll, &QTimer::timeout, [&] {
      if (predicate ()) {
         loop.quit ();
      }
   });
   QObject::connect (&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
   poll.start (10);
   deadline.start (milliseconds);
   loop.exec ();
   return predicate ();
}

TEST (Renderer, ReplacementAnchorCannotReusePreviousItemCard)
{
   QTemporaryDir profile;
   ASSERT_TRUE (profile.isValid ());
   gv::ui::OverlayWindow overlay { {
      .web_dir = std::filesystem::path { GRIMVAULT_TEST_SOURCE_DIR } / "web",
      .user_data_dir = profile.path ().toStdWString (),
   } };
   QRect visible;
   int reveals = 0;
   QObject::connect (&overlay, &gv::ui::OverlayWindow::card_area_changed, [&] (QRect area) {
      visible = area;
      if (!area.isEmpty ()) {
         ++reveals;
      }
   });

   const QRect game { 0, 0, 1600, 1000 };
   gv::api::TooltipLookup lookup;
   lookup.canonical_name = "Bandage";
   lookup.display_name = "Bandage";
   lookup.rarity = "common";
   lookup.pricing.median = 10;
   overlay.anchor_shown (game, {}, { 400, 500 }, true, true, { 500, 100 });
   overlay.present (lookup, game, { 500, 100, 400, 500 }, false);
   ASSERT_TRUE (wait_until ([&] { return !visible.isEmpty (); }, 10000));

   // Reproduce a replacement detection followed immediately by its anchor.
   // No successful analysis belongs to that new anchor, so no card may show.
   overlay.anchor_lost ();
   overlay.anchor_shown (game, {}, { 341, 84 }, true, true, { 1100, 700 });
   EXPECT_TRUE (visible.isEmpty ());
   const int prior_reveals = reveals;
   EXPECT_FALSE (wait_until ([&] { return reveals > prior_reveals; }, 250));
   EXPECT_TRUE (visible.isEmpty ());

   overlay.set_active (false);
   EXPECT_FALSE (overlay.set_active (true));
   EXPECT_FALSE (wait_until ([&] { return reveals > prior_reveals; }, 250));

   // A fresh analysis can still present normally after invalidation.
   overlay.present (lookup, game, { 1100, 700, 341, 84 }, false);
   EXPECT_TRUE (wait_until ([&] { return !visible.isEmpty (); }, 3000));
}

TEST (Renderer, LostAnchorDiscardsPresentationQueuedBeforeRendererReady)
{
   QTemporaryDir profile;
   ASSERT_TRUE (profile.isValid ());
   gv::ui::OverlayWindow overlay { {
      .web_dir = std::filesystem::path { GRIMVAULT_TEST_SOURCE_DIR } / "web",
      .user_data_dir = profile.path ().toStdWString (),
   } };
   int reveals = 0;
   QObject::connect (&overlay, &gv::ui::OverlayWindow::card_area_changed, [&] (QRect area) {
      if (!area.isEmpty ()) {
         ++reveals;
      }
   });
   gv::api::TooltipLookup lookup;
   lookup.canonical_name = "Bandage";
   lookup.rarity = "common";
   const QRect game { 0, 0, 1600, 1000 };
   overlay.anchor_shown (game, {}, { 400, 500 }, true, true, { 500, 100 });
   overlay.present (lookup, game, { 500, 100, 400, 500 }, false);
   overlay.anchor_lost ();
   EXPECT_FALSE (wait_until ([&] { return reveals != 0; }, 3000));
   overlay.set_active (false);
   EXPECT_FALSE (overlay.set_active (true));
}

}

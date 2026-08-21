#include <gv/core/logger.h>
#include <gv/ui/webview_host.h>

#include <QApplication>
#include <QEventLoop>
#include <QImage>
#include <QJsonDocument>
#include <QQuickItem>
#include <QQuickView>
#include <QTemporaryDir>
#include <QTimer>

#include <Windows.h>
#include <objbase.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace {

TEST (Renderer, QmlFallbackLoadsAndScales)
{
   QQuickView view;
   view.setResizeMode (QQuickView::SizeViewToRootObject);
   view.setSource (QUrl { QStringLiteral ("qrc:/qml/Tooltip.qml") });

   ASSERT_EQ (view.status (), QQuickView::Ready);
   auto* root = view.rootObject ();
   ASSERT_NE (root, nullptr);

   const qreal width = root->width ();
   const qreal height = root->height ();
   ASSERT_GT (width, 0);
   ASSERT_GT (height, 0);
   EXPECT_NEAR (view.width (), width, 0.5);
   EXPECT_NEAR (view.height (), height, 0.5);

   root->setProperty ("renderScale", 0.85);
   QCoreApplication::processEvents ();

   EXPECT_NEAR (root->width (), width * 0.85, 0.5);
   EXPECT_NEAR (root->height (), height * 0.85, 0.5);
   EXPECT_NEAR (view.width (), root->width (), 0.5);
   EXPECT_NEAR (view.height (), root->height (), 0.5);
}

TEST (Renderer, QmlFallbackRendersStructuredCard)
{
   QQuickView view;
   view.setResizeMode (QQuickView::SizeViewToRootObject);
   view.setSource (QUrl { QStringLiteral ("qrc:/qml/Tooltip.qml") });

   ASSERT_EQ (view.status (), QQuickView::Ready);
   auto* root = view.rootObject ();
   ASSERT_NE (root, nullptr);
   const qreal empty_height = root->height ();

   const nlohmann::json entity = {
      { "name", "GrimVault" },
      { "realm", "grimvault" },
      { "rarity", "rare" },
      { "sections", nlohmann::json::array ({ {
         { "kind", "analysis" },
         { "item_name", "Bandage" },
         { "item_rarity", "rare" },
         { "tradeable", true },
         { "pricing", {
            { "median", 120 }, { "low", 90 }, { "high", 150 },
            { "confidence", "high" }, { "sample_size", 28 },
         } },
         { "market", { { "sales_30d", 28 }, { "active_listings", 17 } } },
         { "utility", { { "vendor_value", 22 }, { "gear_score", 50 } } },
         { "weighted_roll_score", 82 },
         { "rolls", nlohmann::json::array ({ {
            { "label", "Move Speed" }, { "slot", "secondary" },
            { "formatted_value", "+4" }, { "minimum", 1 }, { "maximum", 5 },
            { "roll_percentile", 75 }, { "grade", "A" },
         } }) },
         { "visible_sections", nlohmann::json::object () },
      } }) },
   };

   const auto document = QJsonDocument::fromJson (
      QByteArray::fromStdString (entity.dump ()));
   ASSERT_TRUE (root->setProperty ("entity", document.toVariant ()));
   EXPECT_GT (root->height (), empty_height);
   view.show ();
   QEventLoop loop;
   QTimer::singleShot (50, &loop, &QEventLoop::quit);
   loop.exec ();

   EXPECT_TRUE (root->property ("analysisMode").toBool ());
   auto* card = root->findChild<QQuickItem*> (QStringLiteral ("card"));
   auto* frame = root->findChild<QQuickItem*> (QStringLiteral ("frame"));
   auto* body = root->findChild<QQuickItem*> (QStringLiteral ("analysisBody"));
   auto* mark = root->findChild<QQuickItem*> (QStringLiteral ("brandMark"));
   auto* market = root->findChild<QQuickItem*> (QStringLiteral ("marketOverview"));
   ASSERT_NE (card, nullptr);
   ASSERT_NE (frame, nullptr);
   ASSERT_NE (body, nullptr);
   ASSERT_NE (mark, nullptr);
   ASSERT_NE (market, nullptr);
   EXPECT_TRUE (body->isVisible ());
   EXPECT_TRUE (market->isVisible ());
   EXPECT_EQ (mark->property ("status").toInt (), 1);
   EXPECT_GT (body->implicitHeight (), 200);
   EXPECT_GT (frame->height (), 200);
   EXPECT_GT (card->height (), 100);
   EXPECT_GT (root->height (), empty_height);
   EXPECT_NEAR (view.height (), root->height (), 0.5);
   const QImage image = view.grabWindow ();
   EXPECT_FALSE (image.isNull ());
   EXPECT_GT (image.width (), 300);
   EXPECT_GT (image.height (), 200);

   auto vendor_entity = entity;
   auto& analysis = vendor_entity ["sections"][0];
   analysis ["pricing"] = {
      { "median", 0 }, { "low", 0 }, { "high", 0 }, { "confidence", "none" },
   };
   analysis ["market"] = nlohmann::json::object ();
   const qreal market_height = root->height ();
   const auto vendor_document = QJsonDocument::fromJson (
      QByteArray::fromStdString (vendor_entity.dump ()));
   ASSERT_TRUE (root->setProperty ("entity", vendor_document.toVariant ()));
   QCoreApplication::processEvents ();

   EXPECT_FALSE (market->isVisible ());
   EXPECT_LT (root->height (), market_height);
}

TEST (Renderer, WebviewCapturesSharedCard)
{
   QTemporaryDir profile;
   ASSERT_TRUE (profile.isValid ());

   QEventLoop loop;
   QTimer timeout;
   timeout.setSingleShot (true);

   bool failed = false;
   bool timed_out = false;
   bool capturing = false;
   std::vector<std::uint8_t> png;
   gv::ui::WebviewHost* renderer = nullptr;

   QObject::connect (&timeout, &QTimer::timeout, [&] {
      timed_out = true;
      loop.quit ();
   });

   auto created = gv::ui::WebviewHost::create (
      gv::ui::WebviewHost::Config {
         .web_dir = std::filesystem::path { GRIMVAULT_TEST_SOURCE_DIR } / "web",
         .user_data_dir = profile.path ().toStdWString (),
      },
      gv::ui::WebviewHost::Callbacks {
         .on_ready = [&] {
            renderer->post_json (nlohmann::json {
               { "type", "render" },
               { "seq", 1 },
               { "entity", {
                  { "name", "GrimVault" },
                  { "realm", "grimvault" },
                  { "rarity", "artifact" },
                  { "sections", nlohmann::json::array ({ {
                     { "kind", "analysis" },
                     { "pricing", {
                        { "median", 1250 },
                        { "low", 1000 },
                        { "high", 1500 },
                        { "confidence", "high" },
                     } },
                     { "roll_score", 84 },
                     { "rolls", nlohmann::json::array () },
                  } }) },
               } },
               { "params", { { "kind", "augment" }, { "compact", true } } },
            }.dump ());
         },
         .on_message = [&] (std::string_view text) {
            const auto message = nlohmann::json::parse (text, nullptr, false);
            if (message.is_discarded () || message.value ("type", "") != "size"
                || message.value ("seq", 0) != 1 || capturing) return;

            capturing = true;
            const int pad = message.value ("pad", 0);
            renderer->resize ({
               message.value ("w", 0) + 2 * pad,
               message.value ("h", 0) + 2 * pad,
            }, 1.0);
            QTimer::singleShot (0, [&] {
               renderer->capture_png ([&] (std::vector<std::uint8_t> bytes) {
                  png = std::move (bytes);
                  loop.quit ();
               });
            });
         },
         .on_process_failed = [&] {
            failed = true;
            loop.quit ();
         },
      });

   ASSERT_TRUE (created.has_value ()) << created.error ().message;
   auto host = std::move (*created);
   renderer = host.get ();

   timeout.start (15000);
   loop.exec ();

   EXPECT_FALSE (failed);
   EXPECT_FALSE (timed_out);
   ASSERT_GT (png.size (), 1000);

   const QImage image = QImage::fromData (
      reinterpret_cast<const uchar*> (png.data ()), static_cast<int> (png.size ()), "PNG");
   EXPECT_FALSE (image.isNull ());
   EXPECT_GT (image.width (), 300);
   EXPECT_GT (image.height (), 100);
}

}

int main (int argc, char** argv)
{
   const HRESULT com = ::CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
   qputenv ("QSG_RENDER_LOOP", "basic");
   QApplication app { argc, argv };
   Q_INIT_RESOURCE (qml);

   QTemporaryDir logs;
   gv::core::Logger::init (logs.path ().toStdWString (), true);
   testing::InitGoogleTest (&argc, argv);
   const int result = RUN_ALL_TESTS ();
   gv::core::Logger::shutdown ();

   if (SUCCEEDED (com)) ::CoUninitialize ();
   return result;
}

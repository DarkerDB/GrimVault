#include <gv/core/logger.h>
#include <gv/ui/webview_host.h>

#include <QApplication>
#include <QEventLoop>
#include <QImage>
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

void captures_shared_card (bool software_rendering)
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
         .software_rendering = software_rendering,
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
         .on_failed = [&] (std::string) {
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

TEST (Renderer, WebviewCapturesSharedCard)
{
   captures_shared_card (false);
}

TEST (Renderer, SoftwareWebviewCapturesSharedCard)
{
   captures_shared_card (true);
}

}

int main (int argc, char** argv)
{
   const HRESULT com = ::CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
   QApplication app { argc, argv };

   QTemporaryDir logs;
   gv::core::Logger::init (logs.path ().toStdWString (), true);
   testing::InitGoogleTest (&argc, argv);
   const int result = RUN_ALL_TESTS ();
   gv::core::Logger::shutdown ();

   if (SUCCEEDED (com)) ::CoUninitialize ();
   return result;
}

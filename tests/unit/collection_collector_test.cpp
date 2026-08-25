#include <gv/collection/collector.h>
#include <gv/collection/log_artifact.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>

TEST (CollectionCollector, RequiresConsentAndDeduplicatesSamples)
{
   std::atomic<int> calls { 0 };
   std::promise<void> sent;
   auto completed = sent.get_future ();
   gv::collection::Collector collector {
      [&calls, &sent] (const gv::api::CollectionSample&) -> gv::core::Result<gv::api::CollectionResult> {
         ++calls;
         sent.set_value ();
         return gv::api::CollectionResult { .accepted = true };
      }
   };
   gv::api::CollectionSample sample {
      .channel = "tooltip",
      .content_type = "image/png",
      .body = "pixels"
   };

   EXPECT_FALSE (collector.submit (sample));
   collector.set_enabled (true);
   EXPECT_TRUE (collector.submit (sample));
   EXPECT_FALSE (collector.submit (sample));
   completed.wait ();
   collector.stop ();
   EXPECT_EQ (calls.load (), 1);
}

TEST (CollectionCollector, SubmitsLatestCompletedDailyLog)
{
   std::promise<gv::api::CollectionSample> sent;
   auto completed = sent.get_future ();
   gv::collection::Collector collector {
      [&sent] (const gv::api::CollectionSample& sample) -> gv::core::Result<gv::api::CollectionResult> {
         sent.set_value (sample);
         return gv::api::CollectionResult { .accepted = true };
      }
   };
   const auto directory = std::filesystem::temp_directory_path ()
      / ("grimvault-collection-" + std::to_string (
         std::chrono::steady_clock::now ().time_since_epoch ().count ()));
   std::filesystem::create_directories (directory);
   {
      std::ofstream { directory / "grimvault_2000-01-01.txt" } << "older";
      std::ofstream { directory / "grimvault_2001-01-01.txt" } << "latest";
      std::ofstream { directory / "unrelated.txt" } << "ignored";
   }

   collector.set_enabled (true);
   EXPECT_TRUE (gv::collection::submit_latest_log (
      collector, directory, "install", "2.1.0", "dev"));
   const auto sample = completed.get ();
   EXPECT_EQ (sample.channel, "log");
   EXPECT_EQ (sample.content_type, "text/plain");
   EXPECT_EQ (sample.body, "latest");
   EXPECT_EQ (sample.metadata ["date"], "2001-01-01");
   EXPECT_EQ (sample.metadata ["filename"], "grimvault_2001-01-01.txt");
   EXPECT_EQ (sample.metadata ["install_id"], "install");
   EXPECT_EQ (sample.metadata ["partition"], "logs");

   collector.stop ();
   std::filesystem::remove_all (directory);
}

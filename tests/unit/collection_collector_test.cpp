#include <gv/collection/collector.h>

#include <gtest/gtest.h>

#include <atomic>
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

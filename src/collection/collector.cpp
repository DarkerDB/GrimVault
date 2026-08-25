#include <gv/collection/collector.h>

#include <gv/core/logger.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gv::collection {

namespace {

std::string fingerprint (const api::CollectionSample& sample)
{
   std::uint64_t hash = 14695981039346656037ull;
   const auto add = [&hash] (std::string_view value) {
      for (const unsigned char byte : value) {
         hash ^= byte;
         hash *= 1099511628211ull;
      }
   };
   add (sample.channel);
   add (sample.body);
   std::ostringstream value;
   value << std::hex << std::setfill ('0') << std::setw (16) << hash;
   return value.str ();
}

}

struct Collector::Impl
{
   explicit Impl (Sender send) : sender (std::move (send)), worker ([this] { run (); }) {}

   Sender sender;
   std::atomic<bool> active { false };
   std::mutex lock;
   std::condition_variable ready;
   std::deque<api::CollectionSample> queue;
   std::unordered_set<std::string> seen;
   std::unordered_map<std::string, std::chrono::steady_clock::time_point> blocked;
   bool stopping = false;
   std::thread worker;

   void run ()
   {
      for (;;) {
         api::CollectionSample sample;
         {
            std::unique_lock guard { lock };
            ready.wait (guard, [this] { return stopping || (active.load () && !queue.empty ()); });
            if (stopping && queue.empty ()) return;
            if (!active.load ()) continue;
            sample = std::move (queue.front ());
            queue.pop_front ();
         }

         auto result = sender (sample);
         if (!result.has_value ()) {
            {
               std::lock_guard guard { lock };
               seen.erase (fingerprint (sample));
            }
            core::Logger::warn ("collection: {} failed: {}", sample.channel, result.error ().message);
            continue;
         }
         if (result->retry_after > 0) {
            std::lock_guard guard { lock };
            blocked [sample.channel] = std::chrono::steady_clock::now ()
               + std::chrono::seconds { result->retry_after };
         }
         if (result->accepted) {
            core::Logger::info ("collection: {} uploaded bytes={} object={}",
               sample.channel, sample.body.size (), result->object_key);
         } else {
            core::Logger::debug ("collection: {} skipped reason={}", sample.channel, result->reason);
         }
      }
   }
};

Collector::Collector (api::DDBClient& client)
   : Collector ([&client] (const api::CollectionSample& sample) { return client.collect (sample); })
{}

Collector::Collector (Sender sender) : impl_ (std::make_unique<Impl> (std::move (sender))) {}

Collector::~Collector () { stop (); }

void Collector::set_enabled (bool enabled)
{
   if (impl_->active.exchange (enabled) == enabled) return;
   if (!enabled) {
      std::lock_guard guard { impl_->lock };
      impl_->queue.clear ();
   }
   impl_->ready.notify_all ();
}

bool Collector::enabled () const noexcept { return impl_->active.load (); }

bool Collector::submit (api::CollectionSample sample)
{
   if (!enabled () || sample.channel.empty () || sample.body.empty ()) return false;
   const auto key = fingerprint (sample);
   std::lock_guard guard { impl_->lock };
   if (const auto found = impl_->blocked.find (sample.channel);
       found != impl_->blocked.end () && found->second > std::chrono::steady_clock::now ()) {
      return false;
   }
   if (impl_->seen.contains (key) || impl_->queue.size () >= 16) return false;
   if (impl_->seen.size () >= 1024) impl_->seen.clear ();
   impl_->seen.insert (key);
   impl_->queue.push_back (std::move (sample));
   impl_->ready.notify_one ();
   return true;
}

void Collector::stop ()
{
   if (!impl_ || !impl_->worker.joinable ()) return;
   {
      std::lock_guard guard { impl_->lock };
      impl_->stopping = true;
      impl_->queue.clear ();
   }
   impl_->ready.notify_all ();
   impl_->worker.join ();
}

}

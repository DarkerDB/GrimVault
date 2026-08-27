#include <gv/app/settings_sync.h>

#include <gv/api/darkerdb_client.h>
#include <gv/auth/session.h>
#include <gv/core/logger.h>
#include <gv/db/repos/user_settings_repo.h>

#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gv::app {

namespace {

   const core::Log log { "settings" };

} // namespace

struct SettingsSync::Impl
{
   gv::api::DDBClient*       api      = nullptr;
   gv::auth::Session*        session  = nullptr;
   gv::db::UserSettingsRepo* repo     = nullptr;
   Config                    cfg;

   QTimer                    timer;
   bool                      running          = false;
   bool                      poll_in_flight   = false;
   bool                      analysis_warmed  = false;
   std::unordered_set<QThread*> workers;

   // Generation counter — each start () bumps this. Worker results from a
   // prior generation are dropped on arrival.
   std::atomic<std::uint64_t> generation { 0 };

   // Current backoff delay. Doubles on failure (clamped to backoff_cap),
   // resets to 0 on success (next tick uses `interval`).
   std::chrono::seconds      current_backoff { 0 };

   void schedule_next (std::chrono::seconds delay)
   {
      timer.start (static_cast<int> (
         std::chrono::duration_cast<std::chrono::milliseconds> (delay).count ()));
   }
};

SettingsSync::SettingsSync (gv::api::DDBClient* api,
                            gv::auth::Session*       session,
                            gv::db::UserSettingsRepo* repo,
                            Config                   cfg,
                            QObject*                 parent)
   : QObject (parent)
   , impl_ (std::make_unique<Impl> ())
{
   impl_->api     = api;
   impl_->session = session;
   impl_->repo    = repo;
   impl_->cfg     = cfg;

   impl_->timer.setSingleShot (true);
   QObject::connect (&impl_->timer, &QTimer::timeout, this, [this] {
      poll_now ();
   });
}

SettingsSync::~SettingsSync ()
{
   stop ();

   // stop() invalidates queued results; waiting here keeps the borrowed API,
   // session, repository, and this object's Impl alive until blocking HTTP
   // calls have actually returned.
   for (auto* worker : impl_->workers) {
      worker->wait ();
      delete worker;
   }
   impl_->workers.clear ();
}

void SettingsSync::start ()
{
   if (impl_->running) return;
   impl_->running         = true;
   impl_->current_backoff = std::chrono::seconds { 0 };
   impl_->generation.fetch_add (1, std::memory_order_relaxed);
   log.info ("settings sync: started (interval={}s)", impl_->cfg.interval.count ());
   poll_now ();
}

void SettingsSync::stop ()
{
   if (!impl_->running) return;
   impl_->running = false;
   impl_->timer.stop ();
   impl_->generation.fetch_add (1, std::memory_order_relaxed);  // invalidate in-flight worker
   if (impl_->api) impl_->api->cancel_pending ();
   log.info ("settings sync: stopped");
}

void SettingsSync::poll_now ()
{
   if (!impl_->running) return;
   if (impl_->poll_in_flight) return;
   if (!impl_->session || !impl_->session->signed_in ()) {
      log.debug ("settings sync: skip (not signed in)");
      impl_->schedule_next (impl_->cfg.interval);
      return;
   }

   impl_->poll_in_flight = true;
   const auto gen = impl_->generation.load (std::memory_order_relaxed);

   auto* worker = QThread::create ([this, gen] {
      auto bundle = impl_->api->get_settings ();
      // Prime the dedicated analysis connection before the first hover, so
      // DNS/TCP/TLS setup happens during background startup work. ping() uses
      // the same persistent curl lane as /analyze.
      if (!impl_->analysis_warmed && bundle.has_value ()) {
         auto warm = impl_->api->ping ();
         if (warm.has_value ()) impl_->analysis_warmed = true;
      }
      QMetaObject::invokeMethod (this,
         [this, gen, bundle = std::move (bundle)] () mutable {
            impl_->poll_in_flight = false;

            // Generation mismatch → caller called stop () + start () again.
            if (gen != impl_->generation.load (std::memory_order_relaxed)) {
               if (impl_->running) QTimer::singleShot (0, this, [this] { poll_now (); });
               return;
            }
            if (!impl_->running) return;

            if (!bundle.has_value ()) {
               auto next = impl_->current_backoff.count () == 0
                  ? impl_->cfg.backoff_floor
                  : std::min (impl_->current_backoff * 2, impl_->cfg.backoff_cap);
               impl_->current_backoff = next;
               log.warn ("settings sync: poll failed: {} (next in {}s)",
                  bundle.error ().message, next.count ());
               emit poll_failed (QString::fromStdString (bundle.error ().message));
               if (!impl_->session || !impl_->session->signed_in ()) {
                  emit authentication_required ();
               }
               impl_->schedule_next (next);
               return;
            }

            // Diff against repo, write only changed keys.
            auto current = impl_->repo->all ();
            const std::unordered_map<std::string, std::string> empty;
            const auto& existing = current.has_value () ? *current : empty;

            int changed = 0;

            for (const auto& [key, value] : existing) {
               if (!is_managed_setting (key) || bundle->values.contains (key)) continue;

               if (auto erased = impl_->repo->erase (key); !erased.has_value ()) {
                  log.warn ("settings sync: erase failed for {}: {}",
                     key, erased.error ().message);
                  continue;
               }
               ++changed;
               log.info ("settings diff: {}: '{}' -> '<unset>'", key, value);
               emit settings_changed (QString::fromStdString (key), {});
            }

            std::vector<std::string_view> keys;
            keys.reserve (bundle->values.size ());
            for (const auto& [key, value] : bundle->values) {
               (void) value;
               keys.push_back (key);
            }
            std::sort (keys.begin (), keys.end (), [] (const auto left, const auto right) {
               constexpr std::string_view order = "tooltip:analysis_order";
               if (left == order) return false;
               if (right == order) return true;
               return left < right;
            });

            // Apply widget values before the explicit order marker. This is
            // deterministic even though SettingsBundle stores flat values in
            // an unordered map, and makes first-run/live sync match reload().
            for (const auto key : keys) {
               const std::string key_text { key };
               const auto& value = bundle->values.at (key_text);
               const auto it = existing.find (key_text);
               if (it != existing.end () && it->second == value) continue;
               const auto previous = it == existing.end () ? "<unset>" : it->second;

               auto w = impl_->repo->set (key_text, value);
               if (!w.has_value ()) {
                  log.warn ("settings sync: write failed for {}: {}", key, w.error ().message);
                  continue;
               }
               ++changed;
               log.info ("settings diff: {}: '{}' -> '{}'", key, previous, value);
               emit settings_changed (QString::fromStdString (key_text),
                                      QString::fromStdString (value));
            }

            impl_->current_backoff = std::chrono::seconds { 0 };
            log.info ("settings sync: ok ({} key{} changed of {})",
               changed,
               changed == 1 ? "" : "s",
               bundle->values.size ());
            emit poll_succeeded (changed);
            impl_->schedule_next (impl_->cfg.interval);
         },
         Qt::QueuedConnection);
   });
   impl_->workers.insert (worker);
   QObject::connect (worker, &QThread::finished, this, [this, worker] {
      impl_->workers.erase (worker);
      worker->deleteLater ();
   });
   worker->start ();
}

} // namespace gv::app

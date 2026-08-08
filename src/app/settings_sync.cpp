#include <gv/app/settings_sync.h>

#include <gv/api/darkerdb_client.h>
#include <gv/auth/session.h>
#include <gv/core/logger.h>
#include <gv/db/repos/user_settings_repo.h>

#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <string>
#include <unordered_map>

namespace gv::app {

namespace {

   const core::Log log { "settings" };

} // namespace

struct SettingsSync::Impl
{
   gv::api::DarkerDbClient*  api      = nullptr;
   gv::auth::Session*        session  = nullptr;
   gv::db::UserSettingsRepo* repo     = nullptr;
   Config                    cfg;

   QTimer                    timer;
   bool                      running          = false;
   bool                      poll_in_flight   = false;

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

SettingsSync::SettingsSync (gv::api::DarkerDbClient* api,
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

SettingsSync::~SettingsSync () { stop (); }

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
      QMetaObject::invokeMethod (this,
         [this, gen, bundle = std::move (bundle)] () mutable {
            impl_->poll_in_flight = false;

            // Generation mismatch → caller called stop () + start () again.
            if (gen != impl_->generation.load (std::memory_order_relaxed)) return;
            if (!impl_->running) return;

            if (!bundle.has_value ()) {
               auto next = impl_->current_backoff.count () == 0
                  ? impl_->cfg.backoff_floor
                  : std::min (impl_->current_backoff * 2, impl_->cfg.backoff_cap);
               impl_->current_backoff = next;
               log.warn ("settings sync: poll failed: {} (next in {}s)",
                  bundle.error ().message, next.count ());
               emit poll_failed (QString::fromStdString (bundle.error ().message));
               impl_->schedule_next (next);
               return;
            }

            // Diff against repo, write only changed keys.
            auto current = impl_->repo->all ();
            const std::unordered_map<std::string, std::string> empty;
            const auto& existing = current.has_value () ? *current : empty;

            int changed = 0;
            for (const auto& [key, value] : bundle->values) {
               const auto it = existing.find (key);
               if (it != existing.end () && it->second == value) continue;

               auto w = impl_->repo->set (key, value);
               if (!w.has_value ()) {
                  log.warn ("settings sync: write failed for {}: {}", key, w.error ().message);
                  continue;
               }
               ++changed;
               emit settings_changed (QString::fromStdString (key),
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
   QObject::connect (worker, &QThread::finished, worker, &QObject::deleteLater);
   worker->start ();
}

} // namespace gv::app

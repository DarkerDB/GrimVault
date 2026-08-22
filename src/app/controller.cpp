#include <gv/app/controller.h>

#include <gv/api/darkerdb_client.h>
#include <gv/core/hotkey_manager.h>
#include <gv/core/logger.h>
#include <gv/db/database.h>
#include <gv/db/repos/user_hotkeys_repo.h>
#include <gv/db/repos/user_settings_repo.h>
#include <gv/ocr/game_locale.h>
#include <gv/ocr/pipeline.h>
#include <gv/ui/debug_overlay.h>
#include <gv/ui/overlay_window.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <QDesktopServices>
#include <QPointer>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iomanip>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

namespace gv::app {

namespace {

   struct DefaultRow { const char* action; const char* accelerator; };

   constexpr std::array<DefaultRow, 3> k_defaults = {{
      { Actions::k_scan_now,       DefaultAccelerators::scan_now       },
      { Actions::k_toggle_overlay,  DefaultAccelerators::toggle_overlay  },
      { Actions::k_open_in_browser, DefaultAccelerators::open_in_browser },
   }};

   const char* mode_name (Mode m)
   {
      switch (m) {
         case Mode::Auto:     return "automatic";
         case Mode::Manual:   return "manual";
         case Mode::Disabled: return "disabled";
      }
      return "automatic";
   }

   std::string fingerprint_for (const api::TooltipLookup& lookup)
   {
      std::uint64_t hash = 1469598103934665603ULL;
      const auto add = [&hash] (std::string_view value) {
         for (const unsigned char ch : value) {
            hash ^= ch;
            hash *= 1099511628211ULL;
         }
         hash ^= 0xff;
         hash *= 1099511628211ULL;
      };
      add (lookup.item_id);
      for (const auto& roll : lookup.rolls) {
         add (roll.attribute_id);
         add (roll.formatted_value);
      }
      std::ostringstream out;
      out << std::hex << std::setfill ('0') << std::setw (16) << hash;
      return out.str ();
   }

   // Empty handler = unknown action. Bind and rebind both dispatch through
   // here so a new action can never be bindable from one path and not the
   // other.
   core::HotkeyManager::Handler handler_for (std::string_view action,
                                             QPointer<Controller> self)
   {
      if (action == Actions::k_scan_now)
         return [self] { if (self) self->action_scan_now       (); };
      if (action == Actions::k_toggle_overlay)
         return [self] { if (self) self->action_toggle_overlay (); };
      if (action == Actions::k_open_in_browser)
         return [self] { if (self) self->action_open_in_browser (); };

      return {};
   }

} // namespace

struct Controller::Impl
{
   Controller&            self;
   Dependencies           deps;

   std::atomic<Mode>      mode { Mode::Auto };
   std::atomic<bool>      authenticated { false };
   std::atomic<bool>      performance_mode { false };
   std::mutex             session_lock;
   std::int64_t           session_id = 0;
   std::string            account_id;

   // What overlay:mode last said. toggle_overlay flips between Disabled and
   // this, so a player in Manual who hides and re-shows the card lands back
   // in Manual rather than being silently upgraded to Auto.
   std::atomic<Mode>      configured_mode { Mode::Auto };
   std::atomic<bool>      debug_overlay { false };

   // Last known game window state (updated from tracker thread, read by
   // mouse watcher + main thread).
   std::mutex             state_lock;
   core::WindowEvent      last_event {};

   // Network analysis never blocks an OCR worker. A single latest-only slot
   // coalesces rapid hover changes while an older request is in flight; the
   // generation checks on both sides of the request prevent stale reveals.
   struct AnalysisJob {
      ocr::RecognizedTooltip tooltip;
      std::string            language;
      std::vector<std::string> enabled_widgets;
   };
   std::thread                analysis_thread;
   std::mutex                 analysis_lock;
   std::condition_variable    analysis_ready;
   std::optional<AnalysisJob> pending_analysis;
   bool                       analysis_stopping = false;
   std::mutex                 widgets_lock;
   std::vector<std::string>   enabled_widgets;

   // Cache of currently bound accelerators by action id.
   std::mutex                                  hk_lock;
   std::unordered_map<std::string, std::string> accels;

   // SPA origin and the last item analysed, for the open-in-browser action.
   std::string browse_base;
   std::mutex  browse_lock;
   std::string last_item_id;

   std::mutex  language_lock;
   std::string language_selection;
   std::string game_language { "en" };

   std::string language ()
   {
      std::lock_guard lk { language_lock };
      return game_language;
   }

   // Latest anchor from the pipeline, Qt-thread only. Re-applied when the
   // game window moves so the Augment presenter gets fresh bounds.
   ocr::Pipeline::AnchorEvent live_anchor {};
   bool                       has_live_anchor = false;

   void sync_pipeline ()
   {
      const auto current = mode.load (std::memory_order_relaxed);
      const bool active = authenticated.load (std::memory_order_relaxed)
         && current != Mode::Disabled;

      if (deps.pipeline) {
         deps.pipeline->set_automatic (current == Mode::Auto);
         deps.pipeline->set_enabled (active);
      }

      if (active) return;

      if (deps.api) deps.api->cancel_pending ();

      {
         std::lock_guard lk { analysis_lock };
         pending_analysis.reset ();
      }
      has_live_anchor = false;
      {
         std::lock_guard lk { browse_lock };
         last_item_id.clear ();
      }
      if (deps.overlay) deps.overlay->clear ();
      if (deps.debug) deps.debug->clear_anchor ();
   }

   void start_session (std::string principal)
   {
      const auto current_language = language ();
      std::lock_guard lk { session_lock };
      account_id = std::move (principal);
      if (!deps.db || account_id.empty () || session_id != 0) return;
      try {
         SQLite::Statement insert { deps.db->sqlite (), R"sql(
            INSERT INTO session_runs (started_at, language, account_id)
            VALUES (unixepoch (), ?, ?)
         )sql" };
         insert.bind (1, current_language);
         insert.bind (2, account_id);
         insert.exec ();
         session_id = deps.db->sqlite ().getLastInsertRowid ();
      } catch (const std::exception& e) {
         core::Logger::warn ("controller: session start failed: {}", e.what ());
      }
   }

   void end_session ()
   {
      std::lock_guard lk { session_lock };
      if (!deps.db || session_id == 0) {
         session_id = 0;
         account_id.clear ();
         return;
      }
      try {
         SQLite::Statement update { deps.db->sqlite (),
            "UPDATE session_runs SET ended_at = unixepoch () WHERE session_id = ?" };
         update.bind (1, session_id);
         update.exec ();
      } catch (const std::exception& e) {
         core::Logger::warn ("controller: session close failed: {}", e.what ());
      }
      session_id = 0;
      account_id.clear ();
   }

   void apply_anchor ()
   {
      core::WindowEvent ev;
      {
         std::lock_guard lk { state_lock };
         ev = last_event;
      }
      if (!ev.visible) return;

      const QRect  game   { ev.bounds.x, ev.bounds.y, ev.bounds.w, ev.bounds.h };
      const QPoint offset { live_anchor.offset_x, live_anchor.offset_y };
      const QSize  tip    { live_anchor.w, live_anchor.h };
      const QPoint pin    { live_anchor.pin_x, live_anchor.pin_y };

      if (deps.overlay) deps.overlay->anchor_shown (
         game, offset, tip, live_anchor.pinned_x, live_anchor.pinned_y, pin);
      if (deps.debug && debug_overlay.load ()) {
         deps.debug->set_anchor (
            offset, tip, live_anchor.pinned_x, live_anchor.pinned_y, pin);
      }
   }

   explicit Impl (Controller& s, Dependencies d) : self (s), deps (std::move (d)) {}

   ~Impl ()
   {
      stop_analysis_worker ();
   }

   void start_analysis_worker ()
   {
      analysis_thread = std::thread { [this] { analysis_loop (); } };
   }

   void stop_analysis_worker ()
   {
      if (deps.api) deps.api->cancel_pending ();
      {
         std::lock_guard lk { analysis_lock };
         analysis_stopping = true;
         pending_analysis.reset ();
      }
      analysis_ready.notify_one ();
      if (analysis_thread.joinable ()) analysis_thread.join ();
   }

   void enqueue_analysis (const ocr::RecognizedTooltip& rt)
   {
      if (deps.api) deps.api->cancel_analysis ();
      const auto current_language = language ();
      std::vector<std::string> widgets;
      {
         std::lock_guard lk { widgets_lock };
         widgets = enabled_widgets;
      }
      {
         std::lock_guard lk { analysis_lock };
         pending_analysis = AnalysisJob { rt, current_language, std::move (widgets) };
      }
      analysis_ready.notify_one ();
   }

   void analysis_loop ()
   {
      for (;;) {
         AnalysisJob job;
         {
            std::unique_lock lk { analysis_lock };
            analysis_ready.wait (lk, [this] {
               return analysis_stopping || pending_analysis.has_value ();
            });
            if (analysis_stopping) return;
            job = std::move (*pending_analysis);
            pending_analysis.reset ();
         }

         if (!authenticated.load (std::memory_order_relaxed)
             || mode.load (std::memory_order_relaxed) == Mode::Disabled) continue;
         if (!deps.api) continue;
         if (deps.pipeline && !deps.pipeline->is_current (job.tooltip.generation)) {
            deps.pipeline->record_evidence (job.tooltip.generation, "analysis_discarded", {
               { "reason", "stale_before_request" },
            });
            continue;
         }

         const auto analysis_started = std::chrono::steady_clock::now ();
         auto result = deps.api->analyze_tooltip (
            job.tooltip.text, job.language, job.tooltip.confidence,
            capture::backend_name (job.tooltip.backend), job.tooltip.gems,
            job.enabled_widgets);
         const auto analysis_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now () - analysis_started).count ();

         if (!authenticated.load (std::memory_order_relaxed)
             || mode.load (std::memory_order_relaxed) == Mode::Disabled) continue;
         if (deps.pipeline && !deps.pipeline->is_current (job.tooltip.generation)) {
            core::log::ocr.event ("analysis_discarded", {
               { "reason", "stale_generation" },
               { "generation", std::to_string (job.tooltip.generation) },
            });
            deps.pipeline->record_evidence (job.tooltip.generation, "analysis_discarded", {
               { "reason", "stale_after_request" },
            });
            continue;
         }
         if (!result.has_value ()) {
            core::log::api.event ("analysis.failed", {
               { "generation", std::to_string (job.tooltip.generation) },
               { "elapsed_ms", std::to_string (analysis_ms) },
               { "kind", std::string { core::kind_name (result.error ().kind) } },
            });
            if (deps.pipeline) deps.pipeline->record_evidence (
               job.tooltip.generation, "analysis_failed", {
                  { "message", result.error ().message },
                  { "elapsed_ms", std::to_string (analysis_ms) },
               });
            continue;
         }

         auto lookup = std::move (*result);
         core::log::api.event ("analysis.ready", {
            { "generation", std::to_string (job.tooltip.generation) },
            { "item_id", lookup.item_id },
            { "request_id", lookup.request_id },
            { "capture_backend", std::string { capture::backend_name (job.tooltip.backend) } },
            { "comps", std::to_string (lookup.pricing.sample_size) },
            { "confidence", lookup.pricing.confidence },
            { "elapsed_ms", std::to_string (analysis_ms) },
         });
         if (deps.pipeline) deps.pipeline->record_evidence (
            job.tooltip.generation, "analysis_ready", {
               { "item_id", lookup.item_id },
               { "request_id", lookup.request_id },
               { "confidence", lookup.pricing.confidence },
               { "elapsed_ms", std::to_string (analysis_ms) },
            });
         persist_find (lookup);
         {
            std::lock_guard lk { browse_lock };
            last_item_id = lookup.item_id;
         }

         QPointer<Controller> guard { &self };
         QMetaObject::invokeMethod (&self,
            [guard, lookup = std::move (lookup), rect = job.tooltip.rect,
             generation = job.tooltip.generation] () mutable {
               if (!guard || !guard->impl_->deps.overlay) return;
               if (!guard->impl_->authenticated.load ()) return;
               if (guard->impl_->mode.load () == Mode::Disabled) return;
               if (guard->impl_->deps.pipeline
                   && !guard->impl_->deps.pipeline->is_current (generation)) return;
               if (!guard->impl_->has_live_anchor
                   || guard->impl_->live_anchor.generation != generation) return;

               core::WindowRect bounds;
               bool active;
               {
                  std::lock_guard lk { guard->impl_->state_lock };
                  bounds = guard->impl_->last_event.bounds;
                  active = guard->impl_->last_event.visible
                        && guard->impl_->last_event.focused;
               }
               if (!active) return;

               const QRect game {
                  bounds.x, bounds.y, bounds.w, bounds.h
               };
               const QRect anchor {
                  bounds.x + rect.x, bounds.y + rect.y, rect.w, rect.h
               };

               // This is the first and only visible render for the generation:
               // complete API data has arrived and the hidden renderer will
               // still wait for its final size + bitmap before revealing it.
               guard->impl_->deps.overlay->present (lookup, game, anchor, true);
               core::log::ui.event ("overlay.presented", {
                  { "generation", std::to_string (generation) },
                  { "item_id", lookup.item_id },
               });
               if (guard->impl_->deps.pipeline) guard->impl_->deps.pipeline->record_evidence (
                  generation, "overlay_presented", {
                     { "item_id", lookup.item_id },
                  });
               emit guard->overlayPresented ();
            }, Qt::QueuedConnection);
      }
   }

   void persist_find (const api::TooltipLookup& lookup)
   {
      if (!deps.db) return;
      std::int64_t active_session = 0;
      {
         std::lock_guard lk { session_lock };
         active_session = session_id;
      }
      if (active_session == 0) return;
      try {
         nlohmann::json attrs {
            { "item_id", lookup.item_id },
            { "quantity", lookup.quantity },
            { "rolls", nlohmann::json::array () },
         };
         for (const auto& roll : lookup.rolls) {
            attrs ["rolls"].push_back ({
               { "attribute_id", roll.attribute_id },
               { "formatted_value", roll.formatted_value },
               { "roll_percentile", roll.roll_percentile },
            });
         }
         SQLite::Statement ins { deps.db->sqlite (), R"sql(
            INSERT INTO item_finds (
               session_id, found_at, canonical_name, locres_hash, rarity,
               attrs_json, market_price, vendor_price, fingerprint
            ) VALUES (
               ?, unixepoch (), ?, '', ?, ?, ?, ?, ?
            )
         )sql" };

         ins.bind (1, active_session);
         ins.bind (2, lookup.canonical_name);
         ins.bind (3, lookup.rarity);
         ins.bind (4, attrs.dump ());
         ins.bind (5, static_cast<long long> (lookup.pricing.median));
         ins.bind (6, static_cast<long long> (lookup.utility.vendor_value));
         ins.bind (7, fingerprint_for (lookup));
         ins.exec ();
      } catch (const std::exception& e) {
         core::Logger::warn ("controller: item_finds insert failed: {}", e.what ());
      }
   }
};

Controller::Controller (Dependencies deps, QObject* parent)
   : QObject (parent), impl_ (std::make_unique<Impl> (*this, std::move (deps)))
{
   qRegisterMetaType<Mode> ("gv::app::Mode");

   const bool highlights_enabled = impl_->deps.highlight_game
                                || impl_->deps.highlight_objects;
   impl_->debug_overlay.store (highlights_enabled);
   if (impl_->deps.debug) {
      impl_->deps.debug->set_highlights (
         impl_->deps.highlight_game, impl_->deps.highlight_objects);
      impl_->deps.debug->set_enabled (highlights_enabled);
   }

   set_language ("automatic");
   if (impl_->deps.pipeline) {
      impl_->sync_pipeline ();
   }

   // Anchor events -> Augment card and (debug mode) the region overlay.
   // Fired from the vision thread; marshalled to the Qt thread.
   if (impl_->deps.pipeline) {
      QPointer<Controller> self { this };

      impl_->deps.pipeline->on_anchor (
         [self] (const ocr::Pipeline::AnchorEvent& ev) {
            if (!self) return;
            QMetaObject::invokeMethod (self, [self, ev] {
               if (!self) return;
               self->impl_->live_anchor     = ev;
               self->impl_->has_live_anchor = true;
               self->impl_->apply_anchor ();
            }, Qt::QueuedConnection);
         });

      impl_->deps.pipeline->on_anchor_lost ([self] (bool immediate) {
         if (!self) return;
         QMetaObject::invokeMethod (self, [self, immediate] {
            if (!self) return;
            self->impl_->has_live_anchor = false;
            if (self->impl_->deps.overlay) self->impl_->deps.overlay->anchor_lost (immediate);
            if (self->impl_->deps.debug)   self->impl_->deps.debug->clear_anchor ();
         }, Qt::QueuedConnection);
      });
   }

   impl_->start_analysis_worker ();
}

Controller::~Controller ()
{
   stop ();
   impl_->end_session ();
}

Mode Controller::mode () const noexcept { return impl_->mode.load (); }

void Controller::set_enabled_widgets (std::vector<std::string> widgets)
{
   std::sort (widgets.begin (), widgets.end ());
   widgets.erase (std::unique (widgets.begin (), widgets.end ()), widgets.end ());
   std::lock_guard lk { impl_->widgets_lock };
   impl_->enabled_widgets = std::move (widgets);
}

void Controller::set_authenticated (bool authenticated, std::string principal)
{
   const bool was_authenticated = impl_->authenticated.load ();
   bool same_account = false;
   {
      std::lock_guard lk { impl_->session_lock };
      same_account = principal == impl_->account_id;
   }
   if (was_authenticated == authenticated && (!authenticated || same_account)) return;

   if (!authenticated) {
      impl_->authenticated.store (false);
      impl_->sync_pipeline ();
      impl_->end_session ();
      return;
   }

   // Treat an account switch as a complete sign-out boundary. This cancels
   // old bearer requests and invalidates the current OCR generation before
   // the new account is allowed to capture or persist anything.
   if (was_authenticated && !same_account) {
      impl_->authenticated.store (false);
      impl_->sync_pipeline ();
      impl_->end_session ();
   }

   impl_->start_session (std::move (principal));
   impl_->authenticated.store (true);
   impl_->sync_pipeline ();
}

void Controller::stop ()
{
   if (!impl_) return;
   impl_->authenticated.store (false, std::memory_order_relaxed);
   impl_->sync_pipeline ();
   impl_->stop_analysis_worker ();
   impl_->end_session ();
}

void Controller::set_configured_mode (Mode m)
{
   // Only act when the DASHBOARD value actually moved. Every settings poll
   // pushes the whole bundle, so comparing against the live mode instead
   // would make an unrelated change (nudging opacity) silently undo an
   // overlay the player had just hidden with the toggle_overlay hotkey.
   if (impl_->configured_mode.exchange (m) == m) return;

   if (impl_->mode.load () == m) return;

   impl_->mode.store (m);
   impl_->sync_pipeline ();
   core::Logger::info ("controller: overlay mode → {}", mode_name (m));

   // Leaving Auto/Manual for Disabled has to take down whatever is on
   // screen; the pipeline keeps running either way.
   if (m == Mode::Disabled && impl_->deps.overlay) impl_->deps.overlay->clear ();

   emit modeChanged (m);
}

void Controller::set_capture_fps (int fps)
{
   if (impl_->deps.pipeline) impl_->deps.pipeline->set_capture_fps (fps);
}

void Controller::set_capture_mode (capture::CaptureMode mode)
{
   if (impl_->deps.pipeline) impl_->deps.pipeline->set_capture_mode (mode);
}

void Controller::set_performance_mode (bool on)
{
   impl_->performance_mode.store (on, std::memory_order_relaxed);
   if (impl_->deps.pipeline) impl_->deps.pipeline->set_performance_mode (on);
}

void Controller::set_language (std::string selection)
{
   std::string source { "setting" };
   std::string normalized { "automatic" };
   std::optional<std::string> resolved;

   if (selection == "automatic") {
      resolved = ocr::detect_game_locale ();
      source = resolved.has_value () ? "game" : "fallback";
   } else if (auto locale = ocr::canonical_locale (selection)) {
      normalized = *locale;
      resolved = std::move (locale);
   } else {
      core::Logger::warn ("controller: unsupported game language setting '{}'", selection);
      source = "fallback";
   }

   if (!resolved) resolved = "en";

   {
      std::lock_guard lk { impl_->language_lock };
      if (impl_->language_selection == normalized && impl_->game_language == *resolved) return;
      impl_->language_selection = std::move (normalized);
      impl_->game_language = *resolved;
   }

   const auto family = ocr::family_of (*resolved);
   if (impl_->deps.pipeline) impl_->deps.pipeline->set_language (family);
   core::Logger::info ("controller: game language '{}' source={} (ocr family '{}')",
      *resolved, source, std::string { ocr::family_dir (family) });
}

std::string Controller::accelerator_for (std::string_view action) const
{
   std::lock_guard lk { impl_->hk_lock };
   if (auto it = impl_->accels.find (std::string { action }); it != impl_->accels.end ()) {
      return it->second;
   }
   for (const auto& row : k_defaults) {
      if (action == row.action) return row.accelerator;
   }
   return {};
}

int Controller::bind_hotkeys_from_repo ()
{
   if (!impl_->deps.hotkeys || !impl_->deps.hotkeys_repo) return 0;

   auto saved = impl_->deps.hotkeys_repo->all ();
   std::unordered_map<std::string, std::string> rows;
   if (saved.has_value ()) rows = *saved;

   // Apply defaults for any missing rows.
   for (const auto& row : k_defaults) {
      if (!rows.contains (row.action)) {
         rows.emplace (row.action, row.accelerator);
         (void) impl_->deps.hotkeys_repo->set (row.action, row.accelerator);
      }
   }

   QPointer<Controller> self { this };
   int bound = 0;

   for (const auto& [action, accel] : rows) {
      auto handler = handler_for (action, self);
      if (!handler) {
         core::Logger::debug ("controller: unknown hotkey action '{}', skipping", action);
         continue;
      }

      auto r = impl_->deps.hotkeys->bind (action, accel, std::move (handler));
      if (r.has_value ()) {
         ++bound;
         { std::lock_guard lk { impl_->hk_lock }; impl_->accels [action] = accel; }
      } else {
         core::Logger::warn ("controller: bind failed for {} → {}: {}",
            action, accel, r.error ().message);
      }
   }

   emit hotkeysChanged ();
   return bound;
}

core::Result<void> Controller::rebind_hotkey (std::string action, std::string accelerator)
{
   if (!impl_->deps.hotkeys || !impl_->deps.hotkeys_repo) {
      return core::fail (core::Error::make (core::ErrorKind::Internal,
         "controller: hotkeys not initialized"));
   }

   QPointer<Controller> self { this };

   auto handler = handler_for (action, self);
   if (!handler) {
      return core::fail (core::Error::make (core::ErrorKind::InvalidArgument,
         "controller: unknown action '{}'", action));
   }

   // Nothing to do when the dashboard re-sends an accelerator we already
   // hold: rebinding would unregister and re-register the same hotkey on
   // every settings poll.
   {
      std::lock_guard lk { impl_->hk_lock };
      if (auto it = impl_->accels.find (action);
          it != impl_->accels.end () && it->second == accelerator) {
         return {};
      }
   }

   auto bind_r = impl_->deps.hotkeys->bind (action, accelerator, std::move (handler));
   if (!bind_r.has_value ()) return bind_r;

   auto persist = impl_->deps.hotkeys_repo->set (action, accelerator);
   if (!persist.has_value ()) {
      core::Logger::warn ("controller: rebind succeeded but persist failed: {}",
         persist.error ().message);
   }

   { std::lock_guard lk { impl_->hk_lock }; impl_->accels [action] = accelerator; }

   emit hotkeysChanged ();
   return {};
}

void Controller::action_scan_now ()
{
   QMetaObject::invokeMethod (this, [this] {
      if (impl_->authenticated.load ()
          && impl_->mode.load () != Mode::Disabled && impl_->deps.pipeline) {
         core::Logger::info ("hotkey: scan_now");
         impl_->deps.pipeline->request_immediate_scan ();
      }
   }, Qt::QueuedConnection);
}

void Controller::set_browse_base (std::string url)
{
   std::lock_guard lk { impl_->browse_lock };
   impl_->browse_base = std::move (url);
}

void Controller::action_open_in_browser ()
{
   std::string url;
   {
      std::lock_guard lk { impl_->browse_lock };
      if (impl_->browse_base.empty () || impl_->last_item_id.empty ()) {
         core::Logger::info ("hotkey: open in browser — nothing analysed yet");
         return;
      }
      url = impl_->browse_base + "/items/" + impl_->last_item_id;
   }

   core::Logger::info ("hotkey: opening {}", url);
   QMetaObject::invokeMethod (this, [url] {
      QDesktopServices::openUrl (QUrl (QString::fromStdString (url)));
   }, Qt::QueuedConnection);
}

void Controller::action_toggle_overlay ()
{
   QMetaObject::invokeMethod (this, [this] {
      Mode next = Mode::Disabled;

      if (impl_->mode.load () == Mode::Disabled) {
         // Coming back on: return to whatever the dashboard configured, so a
         // player in Manual isn't silently upgraded to Auto. When the
         // dashboard is itself what turned the overlay off there is nothing
         // to restore, and refusing to come back would make the hotkey look
         // broken — fall back to Auto.
         const Mode configured = impl_->configured_mode.load ();
         next = configured == Mode::Disabled ? Mode::Auto : configured;
      }

      impl_->mode.store (next);
      impl_->sync_pipeline ();
      core::Logger::info ("hotkey: overlay → {}", mode_name (next));

      if (next == Mode::Disabled && impl_->deps.overlay) impl_->deps.overlay->clear ();
      emit modeChanged (next);
   }, Qt::QueuedConnection);
}

void Controller::on_window_event (const core::WindowEvent& ev)
{
   core::WindowRect prev;
   bool was_active;
   {
      std::lock_guard lk { impl_->state_lock };
      prev = impl_->last_event.bounds;
      was_active = impl_->last_event.visible && impl_->last_event.focused;
      impl_->last_event = ev;
   }

   const bool active = ev.visible && ev.focused;
   const bool moved  = prev.x != ev.bounds.x || prev.y != ev.bounds.y
                    || prev.w != ev.bounds.w || prev.h != ev.bounds.h;

   // Marshal to main thread; pipeline.set_active_window is safe to call here
   // (atomic), but overlay reposition / hide must be on the Qt thread.
   QPointer<Controller> self { this };
   QMetaObject::invokeMethod (this, [self, ev, active, moved, was_active] {
      if (!self) return;
      if (self->impl_->deps.pipeline) {
         self->impl_->deps.pipeline->set_active_window (ev.visible ? ev.hwnd : nullptr);
      }

      if (active && (moved || !was_active) && self->impl_->has_live_anchor) {
         self->impl_->apply_anchor ();
      }
      if (self->impl_->deps.overlay) {
         if (!ev.visible) self->impl_->deps.overlay->clear ();
         if (self->impl_->deps.overlay->set_active (active)) {
            emit self->overlayPresented ();
         }
      }

      // Debug overlay tracks the capture region: visible whenever the game
      // window is (capture works unfocused via WGC).
      if (self->impl_->deps.debug && self->impl_->debug_overlay.load ()) {
         self->impl_->deps.debug->set_region (
            QRect { ev.bounds.x, ev.bounds.y, ev.bounds.w, ev.bounds.h },
            ev.visible);
      }

      emit self->gameWindowChanged (
         QRect { ev.bounds.x, ev.bounds.y, ev.bounds.w, ev.bounds.h }, active);
   }, Qt::QueuedConnection);
}

void Controller::on_tooltip (const ocr::RecognizedTooltip& rt)
{
   if (!impl_->authenticated.load (std::memory_order_relaxed)
       || impl_->mode.load (std::memory_order_relaxed) == Mode::Disabled) return;

   if (!impl_->deps.api || rt.preliminary) return;
   if (impl_->deps.pipeline && !impl_->deps.pipeline->is_current (rt.generation)) return;
   impl_->enqueue_analysis (rt);
}

} // namespace gv::app

#include <gv/app/controller.h>

#include <gv/api/darkerdb_client.h>
#include <gv/core/hotkey_manager.h>
#include <gv/core/logger.h>
#include <gv/db/database.h>
#include <gv/db/repos/user_hotkeys_repo.h>
#include <gv/db/repos/user_settings_repo.h>
#include <gv/ocr/pipeline.h>
#include <gv/ui/debug_overlay.h>
#include <gv/ui/overlay_window.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <QPointer>
#include <QTimer>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace gv::app {

namespace {

   constexpr int k_mouse_still_ms = 100;
   constexpr int k_mouse_poll_ms  = 16;     // ~60 Hz

   struct DefaultRow { const char* action; const char* accelerator; };

   constexpr std::array<DefaultRow, 4> k_defaults = {{
      { Actions::k_scan_now,      DefaultAccelerators::scan_now      },
      { Actions::k_toggle_mode,   DefaultAccelerators::toggle_mode   },
      { Actions::k_debug_toggle,  DefaultAccelerators::debug_toggle  },
      { Actions::k_clear_overlay, DefaultAccelerators::clear_overlay },
   }};

   bool point_in_rect (const POINT& p, const core::WindowRect& r)
   {
      return p.x >= r.x && p.x < r.x + r.w
          && p.y >= r.y && p.y < r.y + r.h;
   }

} // namespace

struct Controller::Impl
{
   Controller&            self;
   Dependencies           deps;

   std::atomic<Mode>      mode { Mode::Auto };
   std::atomic<bool>      debug_overlay { false };

   // Last known game window state (updated from tracker thread, read by
   // mouse watcher + main thread).
   std::mutex             state_lock;
   core::WindowEvent      last_event {};

   // Mouse-still watcher thread.
   std::thread            mouse_thread;
   std::atomic<bool>      mouse_running { false };

   // Network analysis never blocks an OCR worker. A single latest-only slot
   // coalesces rapid hover changes while an older request is in flight; the
   // generation checks on both sides of the request prevent stale reveals.
   struct AnalysisJob {
      ocr::RecognizedTooltip tooltip;
      std::string            language;
   };
   std::thread                analysis_thread;
   std::mutex                 analysis_lock;
   std::condition_variable    analysis_ready;
   std::optional<AnalysisJob> pending_analysis;
   bool                       analysis_stopping = false;

   // Cache of currently bound accelerators by action id.
   std::mutex                                  hk_lock;
   std::unordered_map<std::string, std::string> accels;

   // Game locale for OCR model selection and the lookup `language` param
   // (`game:language` setting; read at startup, applied to the pipeline).
   std::string game_language { "en" };

   // Latest anchor from the pipeline, Qt-thread only. Re-applied when the
   // game window moves so the Augment presenter gets fresh bounds.
   ocr::Pipeline::AnchorEvent live_anchor {};
   bool                       has_live_anchor = false;

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
      stop_mouse_watcher ();
   }

   void start_analysis_worker ()
   {
      analysis_thread = std::thread { [this] { analysis_loop (); } };
   }

   void stop_analysis_worker ()
   {
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
      {
         std::lock_guard lk { analysis_lock };
         pending_analysis = AnalysisJob { rt, game_language };
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

         if (!deps.api) continue;
         if (deps.pipeline && !deps.pipeline->is_current (job.tooltip.generation)) continue;

         auto result = deps.api->analyze_tooltip (
            job.tooltip.text, job.language, job.tooltip.confidence);

         if (deps.pipeline && !deps.pipeline->is_current (job.tooltip.generation)) {
            core::log::ocr.event ("analysis_discarded", {
               { "reason", "stale_generation" },
               { "generation", std::to_string (job.tooltip.generation) },
            });
            continue;
         }
         if (!result.has_value ()) {
            core::Logger::debug ("controller: analysis failed: {}", result.error ().message);
            continue;
         }

         auto lookup = std::move (*result);
         core::log::api.event ("analysis.ready", {
            { "generation", std::to_string (job.tooltip.generation) },
            { "item_id", lookup.item_id },
            { "request_id", lookup.request_id },
            { "comps", std::to_string (lookup.pricing.sample_size) },
            { "confidence", lookup.pricing.confidence },
         });
         persist_find (lookup);

         QPointer<Controller> guard { &self };
         QMetaObject::invokeMethod (&self,
            [guard, lookup = std::move (lookup), rect = job.tooltip.rect,
             generation = job.tooltip.generation] () mutable {
               if (!guard || !guard->impl_->deps.overlay) return;
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
               emit guard->overlayPresented ();
            }, Qt::QueuedConnection);
      }
   }

   void start_mouse_watcher ()
   {
      mouse_running.store (true);
      mouse_thread = std::thread { [this] { mouse_loop (); } };
   }

   void stop_mouse_watcher ()
   {
      mouse_running.store (false);
      if (mouse_thread.joinable ()) mouse_thread.join ();
   }

   void mouse_loop ()
   {
      POINT last_p {};
      auto  last_change = std::chrono::steady_clock::now ();
      bool  already_fired = false;

      while (mouse_running.load (std::memory_order_relaxed)) {
         std::this_thread::sleep_for (std::chrono::milliseconds (k_mouse_poll_ms));

         if (mode.load () != Mode::Auto) {
            already_fired = false;
            continue;
         }

         POINT p {};
         if (!::GetCursorPos (&p)) continue;

         if (p.x != last_p.x || p.y != last_p.y) {
            last_p = p;
            last_change = std::chrono::steady_clock::now ();
            already_fired = false;
            continue;
         }

         if (already_fired) continue;

         const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
            std::chrono::steady_clock::now () - last_change).count ();

         if (elapsed < k_mouse_still_ms) continue;

         // Check the cursor is inside the game window.
         core::WindowRect bounds;
         bool             visible_and_focused;
         {
            std::lock_guard lk { state_lock };
            bounds = last_event.bounds;
            visible_and_focused = last_event.visible && last_event.focused;
         }

         if (!visible_and_focused)      { already_fired = true; continue; }
         if (!point_in_rect (p, bounds)) { already_fired = true; continue; }

         already_fired = true;
         if (deps.pipeline) deps.pipeline->request_immediate_scan ();
      }
   }

   void persist_find (const api::TooltipLookup& lookup) const
   {
      if (!deps.db) return;
      try {
         SQLite::Statement ins { deps.db->sqlite (), R"sql(
            INSERT INTO item_finds (
               found_at, canonical_name, locres_hash, rarity,
               attrs_json, market_price, vendor_price, fingerprint
            ) VALUES (
               unixepoch (), ?, '', ?, ?, ?, ?, ?
            )
         )sql" };

         ins.bind (1, lookup.canonical_name);
         ins.bind (2, lookup.rarity);
         ins.bind (3, lookup.raw.dump ());
         ins.bind (4, static_cast<long long> (lookup.pricing.median));
         ins.bind (5, static_cast<long long> (lookup.utility.vendor_value));
         ins.bind (6, lookup.canonical_name);   // fingerprint placeholder
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

   if (impl_->deps.settings_repo) {
      if (auto v = impl_->deps.settings_repo->get ("game:language");
          v.has_value () && v->has_value () && !(*v)->empty ()) {
         impl_->game_language = **v;
      }
   }

   if (impl_->deps.pipeline) {
      impl_->deps.pipeline->set_language (ocr::family_of (impl_->game_language));
   }
   core::Logger::info ("controller: game language '{}' (ocr family '{}')",
      impl_->game_language,
      std::string { ocr::family_dir (ocr::family_of (impl_->game_language)) });

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

   impl_->start_mouse_watcher ();
   impl_->start_analysis_worker ();
}

Controller::~Controller () = default;

Mode Controller::mode () const noexcept { return impl_->mode.load (); }

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
      core::HotkeyManager::Handler handler;

      if      (action == Actions::k_scan_now)      handler = [self] { if (self) self->action_scan_now      (); };
      else if (action == Actions::k_toggle_mode)   handler = [self] { if (self) self->action_toggle_mode   (); };
      else if (action == Actions::k_debug_toggle)  handler = [self] { if (self) self->action_debug_toggle  (); };
      else if (action == Actions::k_clear_overlay) handler = [self] { if (self) self->action_clear_overlay (); };
      else {
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
   core::HotkeyManager::Handler handler;

   if      (action == Actions::k_scan_now)      handler = [self] { if (self) self->action_scan_now      (); };
   else if (action == Actions::k_toggle_mode)   handler = [self] { if (self) self->action_toggle_mode   (); };
   else if (action == Actions::k_debug_toggle)  handler = [self] { if (self) self->action_debug_toggle  (); };
   else if (action == Actions::k_clear_overlay) handler = [self] { if (self) self->action_clear_overlay (); };
   else {
      return core::fail (core::Error::make (core::ErrorKind::InvalidArgument,
         "controller: unknown action '{}'", action));
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
      if (impl_->deps.pipeline) {
         core::Logger::info ("hotkey: scan_now");
         impl_->deps.pipeline->request_immediate_scan ();
      }
   }, Qt::QueuedConnection);
}

void Controller::action_toggle_mode ()
{
   QMetaObject::invokeMethod (this, [this] {
      const Mode next = (impl_->mode.load () == Mode::Auto) ? Mode::Manual : Mode::Auto;
      impl_->mode.store (next);
      core::Logger::info ("hotkey: mode → {}", next == Mode::Auto ? "auto" : "manual");
      emit modeChanged (next);
   }, Qt::QueuedConnection);
}

void Controller::action_debug_toggle ()
{
   QMetaObject::invokeMethod (this, [this] {
      if (!impl_->deps.highlight_game && !impl_->deps.highlight_objects) {
         core::Logger::info (
            "hotkey: debug highlights unavailable (start with --debug=highlight:...)");
         return;
      }
      const bool next = !impl_->debug_overlay.load ();
      impl_->debug_overlay.store (next);
      core::Logger::info ("hotkey: debug overlay {}", next ? "on" : "off");

      if (impl_->deps.debug) {
         core::WindowEvent ev;
         {
            std::lock_guard lk { impl_->state_lock };
            ev = impl_->last_event;
         }
         impl_->deps.debug->set_enabled (next);
         impl_->deps.debug->set_region (
            QRect { ev.bounds.x, ev.bounds.y, ev.bounds.w, ev.bounds.h },
            ev.visible);

         if (next && impl_->has_live_anchor) impl_->apply_anchor ();
      }
   }, Qt::QueuedConnection);
}

void Controller::action_clear_overlay ()
{
   QMetaObject::invokeMethod (this, [this] {
      if (impl_->deps.overlay) impl_->deps.overlay->clear ();
      emit overlayCleared ();
   }, Qt::QueuedConnection);
}

void Controller::on_window_event (const core::WindowEvent& ev)
{
   core::WindowRect prev;
   {
      std::lock_guard lk { impl_->state_lock };
      prev = impl_->last_event.bounds;
      impl_->last_event = ev;
   }

   const bool active = ev.visible && ev.focused;
   const bool moved  = prev.x != ev.bounds.x || prev.y != ev.bounds.y
                    || prev.w != ev.bounds.w || prev.h != ev.bounds.h;

   // Marshal to main thread; pipeline.set_active_window is safe to call here
   // (atomic), but overlay reposition / hide must be on the Qt thread.
   QPointer<Controller> self { this };
   QMetaObject::invokeMethod (this, [self, ev, active, moved] {
      if (!self) return;
      if (self->impl_->deps.pipeline) {
         self->impl_->deps.pipeline->set_active_window (active ? ev.hwnd : nullptr);
      }

      // Hide the card when the game loses focus/visibility. A moved or
      // resized window keeps the anchor: re-apply it so the presenter gets
      // the fresh bounds.
      if (!active && self->impl_->deps.overlay) {
         self->impl_->deps.overlay->clear ();
      } else if (moved && self->impl_->has_live_anchor) {
         self->impl_->apply_anchor ();
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
   {
      QPointer<Controller> self { this };
      QMetaObject::invokeMethod (this, [self] {
         if (self) emit self->scanActivity ();
      }, Qt::QueuedConnection);
   }

   if (!impl_->deps.api || rt.preliminary) return;
   if (impl_->deps.pipeline && !impl_->deps.pipeline->is_current (rt.generation)) return;
   impl_->enqueue_analysis (rt);
}

} // namespace gv::app

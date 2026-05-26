#include <gv/app/controller.h>

#include <gv/api/darkerdb_client.h>
#include <gv/core/hotkey_manager.h>
#include <gv/core/logger.h>
#include <gv/db/database.h>
#include <gv/db/repos/user_hotkeys_repo.h>
#include <gv/db/repos/user_settings_repo.h>
#include <gv/ocr/pipeline.h>
#include <gv/ui/overlay_window.h>

#include <SQLiteCpp/SQLiteCpp.h>

#include <QPointer>
#include <QTimer>

#include <Windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
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

   // Cache of currently bound accelerators by action id.
   std::mutex                                  hk_lock;
   std::unordered_map<std::string, std::string> accels;

   explicit Impl (Controller& s, Dependencies d) : self (s), deps (std::move (d)) {}

   ~Impl ()
   {
      stop_mouse_watcher ();
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
         ins.bind (5, static_cast<long long> (lookup.pricing.low));
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
   impl_->start_mouse_watcher ();
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
         impl_->deps.hotkeys_repo->set (row.action, row.accelerator);
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
      const bool next = !impl_->debug_overlay.load ();
      impl_->debug_overlay.store (next);
      core::Logger::info ("hotkey: debug overlay {}", next ? "on" : "off");
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
   {
      std::lock_guard lk { impl_->state_lock };
      impl_->last_event = ev;
   }

   const bool active = ev.visible && ev.focused;

   // Marshal to main thread; pipeline.set_active_window is safe to call here
   // (atomic), but overlay reposition / hide must be on the Qt thread.
   QPointer<Controller> self { this };
   QMetaObject::invokeMethod (this, [self, ev, active] {
      if (!self) return;
      if (self->impl_->deps.pipeline) {
         self->impl_->deps.pipeline->set_active_window (active ? ev.hwnd : nullptr);
      }
      if (!active && self->impl_->deps.overlay && !self->impl_->debug_overlay.load ()) {
         self->impl_->deps.overlay->clear ();
      }
   }, Qt::QueuedConnection);
}

void Controller::on_tooltip (const ocr::RecognizedTooltip& rt)
{
   // Currently called from the OCR worker thread. Do the (blocking) API
   // call here so it doesn't block the Qt event loop, then marshal the
   // resulting present() over.
   if (!impl_->deps.api) return;

   const std::string text { rt.text };
   const int x = rt.rect.x;
   const int y = rt.rect.y;

   auto res = impl_->deps.api->lookup_tooltip (text, "en");
   if (!res.has_value ()) {
      core::Logger::debug ("controller: lookup failed: {}", res.error ().message);
      return;
   }

   auto lookup = std::move (*res);
   impl_->persist_find (lookup);

   QPointer<Controller> self { this };
   QMetaObject::invokeMethod (this, [self, lookup = std::move (lookup), x, y] () mutable {
      if (!self) return;
      if (self->impl_->deps.overlay) {
         self->impl_->deps.overlay->present (lookup, x, y);
         emit self->overlayPresented ();
      }
   }, Qt::QueuedConnection);
}

} // namespace gv::app

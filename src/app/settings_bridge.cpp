#include <gv/app/settings_bridge.h>

#include <gv/app/controller.h>
#include <gv/core/logger.h>
#include <gv/core/startup_link.h>
#include <gv/db/repos/user_settings_repo.h>
#include <gv/ui/overlay_window.h>

#include <map>
#include <utility>
#include <vector>

namespace gv::app {

namespace {

   constexpr const char* k_startup_name = "GrimVault";

} // namespace

struct SettingsBridge::Impl
{
   Dependencies deps;
   Preferences  prefs;

   // Last values actually pushed downstream, so a poll that reports a key
   // changed but lands on the same effective value doesn't re-register a
   // hotkey or rewrite the registry.
   std::string last_startup_exe;
   bool        startup_applied = false;
   bool        last_startup    = false;

   void push_overlay ()
   {
      if (!deps.overlay) return;
      // The card advertises this key wherever it truncates a list, so the
      // render options carry whatever is actually bound right now.
      auto options = prefs.options;
      options.browse_hotkey = prefs.hotkey_open_in_browser.empty ()
         ? std::string { DefaultAccelerators::open_in_browser }
         : prefs.hotkey_open_in_browser;

      deps.overlay->set_layout  (prefs.layout);
      deps.overlay->set_options (options);
   }

   void push_controller ()
   {
      if (!deps.controller) return;

      deps.controller->set_configured_mode (prefs.overlay_mode);
      if (!deps.capture_fps_locked)
         deps.controller->set_capture_fps (prefs.capture_fps);
      deps.controller->set_capture_mode (prefs.capture_mode);
      deps.controller->set_performance_mode (prefs.performance_mode);
      deps.controller->set_language (prefs.language);
      std::vector<std::string> enabled_widgets;
      for (const auto& [widget, enabled] : prefs.options.widgets) {
         if (enabled) enabled_widgets.push_back (widget);
      }
      deps.controller->set_enabled_widgets (std::move (enabled_widgets));

      // A dashboard accelerator overrides the compiled default; an empty
      // one means the server never sent it, so leave the local binding be.
      rebind (Actions::k_scan_now,        prefs.hotkey_scan_now);
      rebind (Actions::k_toggle_overlay,  prefs.hotkey_toggle_overlay);
      rebind (Actions::k_open_in_browser, prefs.hotkey_open_in_browser);
   }

   void rebind (const char* action, const std::string& accelerator)
   {
      if (accelerator.empty ()) return;

      // rebind_hotkey is a no-op when the accelerator is already bound, so
      // this is safe to call on every poll.
      if (auto r = deps.controller->rebind_hotkey (action, accelerator); !r.has_value ()) {
         core::log::app.warn ("settings: hotkey {} → {} rejected: {}",
            action, accelerator, r.error ().message);
      }
   }

   void push_startup ()
   {
      if (deps.exe_path.empty ()) return;
      if (startup_applied && last_startup == prefs.launch_on_startup
          && last_startup_exe == deps.exe_path) {
         return;
      }

      const auto result = prefs.launch_on_startup
         ? core::StartupLink::enable (k_startup_name, deps.exe_path, "--hidden")
         : core::StartupLink::disable (k_startup_name);

      if (!result.has_value ()) {
         core::log::app.warn ("settings: launch-on-startup {} failed: {}",
            prefs.launch_on_startup ? "enable" : "disable", result.error ().message);
         return;
      }

      startup_applied  = true;
      last_startup     = prefs.launch_on_startup;
      last_startup_exe = deps.exe_path;
      core::log::app.info ("settings: launch on startup {}",
         prefs.launch_on_startup ? "enabled" : "disabled");
   }

   void push_all ()
   {
      push_overlay ();
      push_controller ();
      push_startup ();
   }
};

SettingsBridge::SettingsBridge (Dependencies deps, QObject* parent)
   : QObject (parent), impl_ (std::make_unique<Impl> ())
{
   impl_->deps = std::move (deps);
}

SettingsBridge::~SettingsBridge () = default;

const Preferences& SettingsBridge::preferences () const noexcept { return impl_->prefs; }

bool SettingsBridge::auto_updates_enabled () const noexcept
{
   return impl_->prefs.auto_updates && !impl_->deps.updates_locked_off;
}

void SettingsBridge::reload ()
{
   impl_->prefs = Preferences {};

   if (impl_->deps.repo) {
      auto stored = impl_->deps.repo->all ();
      if (stored.has_value ()) {
         // std::map, not the repo's unordered_map: tooltip:analysis:* folds
         // in iteration order and that order becomes the augment's render
         // order, so it has to be stable across runs.
         const std::map<std::string, std::string> ordered { stored->begin (), stored->end () };
         for (const auto& [key, value] : ordered) {
            (void) gv::app::apply (impl_->prefs, key, value);
         }
      } else {
         core::log::app.warn ("settings: read failed: {}", stored.error ().message);
      }
   }

   impl_->push_all ();
   emit applied ();
}

void SettingsBridge::apply (const QString& key, const QString& value)
{
   const auto k = key.toStdString ();
   const auto v = value.toStdString ();

   if (!gv::app::apply (impl_->prefs, k, v)) {
      core::log::app.debug ("settings: ignoring unknown key {}", k);
      return;
   }

   core::log::app.info ("settings applied: {} = {}", k, v.empty () ? "<default>" : v);

   impl_->push_all ();
   emit applied ();
}

} // namespace gv::app

#pragma once

#include <gv/ui/augment_payload.h>
#include <gv/ui/layout.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace gv::app {

// Everything the dashboard controls, in the shape the app consumes it.
//
// SettingsSync mirrors /v2/grimvault/settings into UserSettingsRepo as flat
// colon-namespaced keys; this is the fold of those keys into live values,
// and SettingsBridge is what pushes the result at the overlay, the
// controller and the update service.
//
// Kept Qt-free and header-only so `apply` is unit-testable without an event
// loop: the fold is the part with the interesting edge cases (unknown keys,
// erased keys, out-of-range numbers), not the fan-out.
struct Preferences
{
   enum class OverlayMode { Automatic, Manual, Disabled };

   OverlayMode          overlay_mode = OverlayMode::Automatic;
   gv::ui::Layout       layout;
   gv::ui::augment::Options options;

   bool auto_updates      = true;
   bool launch_on_startup = true;

   // hotkeys:force_refresh and hotkeys:toggle_overlay. Empty means the
   // server never sent one, so the client's local default stands.
   std::string hotkey_scan_now;
   std::string hotkey_toggle_overlay;
   std::string hotkey_open_in_browser;
};

namespace detail {

   inline bool parse_bool (std::string_view v, bool fallback)
   {
      if (v == "true"  || v == "1" || v == "on"  || v == "yes") return true;
      if (v == "false" || v == "0" || v == "off" || v == "no")  return false;
      return fallback;
   }

   inline double parse_double (std::string_view v, double fallback, double lo, double hi)
   {
      // from_chars for doubles is not available on every stdlib we build
      // against; strtod on a NUL-terminated copy is the portable read.
      const std::string text { v };
      char* end = nullptr;
      const double parsed = std::strtod (text.c_str (), &end);
      if (end == text.c_str ()) return fallback;
      return std::clamp (parsed, lo, hi);
   }

   inline int parse_int (std::string_view v, int fallback, int lo, int hi)
   {
      int parsed = 0;
      const auto* first = v.data ();
      const auto* last  = v.data () + v.size ();
      // Tolerate the "20.000000" a double-valued key can arrive as: parse
      // the integral head and ignore any fractional tail.
      const auto [ptr, ec] = std::from_chars (first, last, parsed);
      if (ec != std::errc {} || ptr == first) return fallback;
      return std::clamp (parsed, lo, hi);
   }

   inline gv::ui::Layout::Align parse_align (std::string_view v,
                                             gv::ui::Layout::Align fallback)
   {
      using Align = gv::ui::Layout::Align;
      if (v == "attached")     return Align::Attached;
      if (v == "top_left")     return Align::TopLeft;
      if (v == "top_right")    return Align::TopRight;
      if (v == "bottom_left")  return Align::BottomLeft;
      if (v == "bottom_right") return Align::BottomRight;
      return fallback;
   }

   // Set (or append) one widget toggle, preserving the server's render
   // order. Append rather than reject on an unknown slug: the widget
   // vocabulary lives in grimvault-widgets.yaml, and a client that dropped
   // slugs it hadn't been compiled against would need a release per widget.
   inline void set_widget (gv::ui::augment::Options& options,
                           std::string widget, bool visible)
   {
      for (auto& [slug, current] : options.widgets) {
         if (slug == widget) { current = visible; return; }
      }
      options.widgets.emplace_back (std::move (widget), visible);
   }

} // namespace detail

// Fold one synced key into `out`. An empty `value` means the key was erased
// server-side, so the field reverts to its compiled default.
//
// Returns false for a key this build does not consume — the caller logs it
// rather than treating it as an error, because a newer server is allowed to
// send keys an older client has never heard of.
inline bool apply (Preferences& out, std::string_view key, std::string_view value)
{
   const Preferences fallback {};
   const bool erased = value.empty ();

   using Align = gv::ui::Layout::Align;

   if (key == "overlay:mode") {
      const auto mode =
           value == "manual"   ? Preferences::OverlayMode::Manual
         : value == "disabled" ? Preferences::OverlayMode::Disabled
         : value == "automatic"? Preferences::OverlayMode::Automatic
         : fallback.overlay_mode;
      out.overlay_mode   = erased ? fallback.overlay_mode : mode;
      out.layout.enabled = out.overlay_mode != Preferences::OverlayMode::Disabled;
      return true;
   }
   if (key == "overlay:alignment") {
      out.layout.align = erased ? fallback.layout.align
                                : detail::parse_align (value, Align::Attached);
      return true;
   }
   if (key == "overlay:opacity") {
      out.layout.opacity = erased ? fallback.layout.opacity
         : detail::parse_double (value, fallback.layout.opacity, 0.0, 1.0);
      return true;
   }
   if (key == "overlay:scale") {
      // Floor well above zero: a 0-scale card is an invisible one, and the
      // dashboard slider can't produce it but a hand-written PATCH can.
      out.layout.scale = erased ? fallback.layout.scale
         : detail::parse_double (value, fallback.layout.scale, 0.5, 3.0);
      return true;
   }
   if (key == "overlay:offset_x") {
      out.layout.offset_x = erased ? fallback.layout.offset_x
         : detail::parse_int (value, fallback.layout.offset_x, -4096, 4096);
      return true;
   }
   if (key == "overlay:offset_y") {
      out.layout.offset_y = erased ? fallback.layout.offset_y
         : detail::parse_int (value, fallback.layout.offset_y, -4096, 4096);
      return true;
   }

   if (key.starts_with ("tooltip:analysis:")) {
      const auto widget = key.substr (std::string_view { "tooltip:analysis:" }.size ());
      if (widget.empty ()) return false;
      detail::set_widget (out.options, std::string { widget },
                          erased ? true : detail::parse_bool (value, true));
      return true;
   }

   if (key == "pricing:currency_display") {
      out.options.currency_display =
           erased            ? fallback.options.currency_display
         : value == "compact"? "compact"
         : "absolute";
      return true;
   }

   if (key == "behavior:is_auto_update_enabled") {
      out.auto_updates = erased ? fallback.auto_updates
                                : detail::parse_bool (value, fallback.auto_updates);
      return true;
   }
   if (key == "behavior:is_launch_on_startup_enabled") {
      out.launch_on_startup = erased ? fallback.launch_on_startup
         : detail::parse_bool (value, fallback.launch_on_startup);
      return true;
   }

   if (key == "hotkeys:force_refresh") {
      out.hotkey_scan_now = erased ? std::string {} : std::string { value };
      return true;
   }
   if (key == "hotkeys:toggle_overlay") {
      out.hotkey_toggle_overlay = erased ? std::string {} : std::string { value };
      return true;
   }
   if (key == "hotkeys:open_in_browser") {
      out.hotkey_open_in_browser = erased ? std::string {} : std::string { value };
      return true;
   }

   // tooltip:sections:* and tooltip:is_price_history_sparkline_visible
   // belong to the DarkerDB item tooltip on the website, not the augment.
   // Recognized so they don't log as unknown; nothing here consumes them.
   if (key.starts_with ("tooltip:sections:")
       || key == "tooltip:is_price_history_sparkline_visible") {
      return true;
   }

   return false;
}

} // namespace gv::app

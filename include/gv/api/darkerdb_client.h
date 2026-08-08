#pragma once

#include <gv/core/result.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gv::db   { class Database; }
namespace gv::auth { class Session;   }

namespace gv::api {

// Per contract §4.3, pricing is currency + (low, median, high) + provenance.
// `market` is retained as an alias for `median` so the existing overlay
// keeps rendering until the QML is updated.
struct Pricing {
   std::string       currency    = "gold";
   std::int64_t      low         = 0;
   std::int64_t      median      = 0;
   std::int64_t      high        = 0;
   std::int64_t      sample_size = 0;
   std::int32_t      ttl_seconds = 0;
   std::string       as_of;
   std::int64_t      market      = 0;   // alias for median; kept for overlay compat
   nlohmann::json    raw;
};

struct TooltipAttribute {
   std::string label;
   std::string value;
};

// Server-resolved item. Returned by lookup; parsing and localization
// happen server-side so the client stays language-agnostic.
struct TooltipLookup {
   std::string                    canonical_name;
   std::string                    rarity;
   std::vector<TooltipAttribute>  primary;
   std::vector<TooltipAttribute>  secondary;
   std::vector<TooltipAttribute>  details;
   Pricing                        pricing;
   std::string                    request_id;
   nlohmann::json                 raw;
};

// Dashboard-controlled settings snapshot from /v2/grimvault/settings.
//
// Wire schema (per docs/architecture/grimvault-settings.md) is nested:
//
//    { "overlay":  { "mode", "alignment", "opacity", "scale", "offset_x", "offset_y" },
//      "tooltip":  { "sections": { "header", ... }, "is_price_history_sparkline_visible" },
//      "pricing":  { "currency_display", "source", "window_days" },
//      "behavior": { "is_telemetry_enabled", "is_auto_update_enabled", "is_launch_on_startup_enabled" },
//      "hotkeys":  { "toggle_overlay", "force_refresh" },
//      "updated_at": "..." }
//
// `values` carries the same data flattened to colon-namespaced strings
// (overlay:opacity → "0.9") so that downstream consumers — SettingsSync,
// UserSettingsRepo, CLI — keep working off a flat key/value map without
// having to walk the typed tree. Typed fields are the source of truth;
// `values` is regenerated from them on parse.
//
// `raw` preserves the unmodified server envelope for diagnostics.
struct SettingsBundle {
   struct Overlay {
      std::string  mode      = "automatic";
      std::string  alignment = "attached";
      double       opacity   = 0.9;
      double       scale     = 1.0;
      std::int32_t offset_x  = 0;
      std::int32_t offset_y  = 0;
   };

   struct TooltipSections {
      bool header    = true;
      bool primary   = true;
      bool secondary = true;
      bool details   = true;
      bool quests    = true;
      bool pricing   = true;
   };

   struct Tooltip {
      TooltipSections sections;
      bool            is_price_history_sparkline_visible = true;
   };

   struct Pricing {
      std::string  currency_display = "gold";
      std::string  source           = "market_median";
      std::int32_t window_days      = 7;
   };

   struct Behavior {
      bool is_telemetry_enabled         = true;
      bool is_auto_update_enabled       = true;
      bool is_launch_on_startup_enabled = true;
   };

   struct Hotkeys {
      std::string toggle_overlay = "Ctrl+Shift+G";
      std::string force_refresh  = "F5";
   };

   Overlay   overlay;
   Tooltip   tooltip;
   Pricing   pricing;
   Behavior  behavior;
   Hotkeys   hotkeys;
   std::string updated_at;

   std::unordered_map<std::string, std::string> values;
   nlohmann::json                                raw;
};

// Probe response from /v2/grimvault/ping. Used by CLI `status` / `doctor`.
struct PingResult {
   bool         ok = false;
   std::string  player_id;
   std::string  user_id;
   std::string  env;
   std::string  server_time;
   std::string  request_id;
   nlohmann::json raw;
};

class DarkerDbClient
{
public:
   struct Config {
      std::string base_url;              // from active_env ().api_base_url
      std::string client_id;             // from active_env ().client_id (X-Client-Id)
      std::string user_agent     = "";   // computed if empty
      std::string ca_bundle      = "";
      std::chrono::milliseconds timeout { 15000 };
   };

   // The session is the source of truth for the bearer token. Borrowed; the
   // session must outlive the client.
   DarkerDbClient  (Config cfg, gv::auth::Session* session, gv::db::Database* cache_db = nullptr);
   ~DarkerDbClient ();

   DarkerDbClient (const DarkerDbClient&)            = delete;
   DarkerDbClient& operator= (const DarkerDbClient&) = delete;

   // POST /v2/grimvault/lookup. Sends the OCR text per contract §4.2, parses
   // the response per §4.3. Cached locally for `cache_ttl`.
   core::Result<TooltipLookup> lookup_tooltip (
      std::string_view raw_text,
      std::string_view language,
      std::chrono::seconds cache_ttl = std::chrono::seconds (300)
   );

   // POST /v2/grimvault/ping. Auth probe; no quota consumption per §4.7.
   core::Result<PingResult> ping ();

   // GET /v2/grimvault/settings. Returns the dashboard-controlled settings
   // bundle. Source of truth for any key the server manages.
   core::Result<SettingsBundle> get_settings ();

   // POST /diagnostics. Used by Diagnostics page's "send diagnostics" button.
   core::Result<void> send_diagnostics (const nlohmann::json& payload);

   // Stable Windows machine id (HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid).
   static std::string machine_id ();

   // One-shot curl_global_init / curl_global_cleanup. Call once per process.
   static void global_init    ();
   static void global_cleanup ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::api

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
   std::int64_t      quick_list  = 0;
   std::int64_t      lowest_ask  = 0;
   std::int64_t      total_value = 0;
   std::string       confidence;
   double            mean_similarity = 0.0;
   nlohmann::json    raw;
};

struct TooltipAttribute {
   std::string label;
   std::string value;
};

struct AnalysisRoll {
   std::string           attribute_id;
   std::string           label;
   std::string           slot;
   double                value = 0.0;
   std::string           formatted_value;
   std::optional<double> minimum;
   std::optional<double> maximum;
   std::optional<int>    roll_percentile;
   std::string           grade;
};

struct GemChange {
   std::string replace_attribute_id;
   std::string replace_label;
   std::string replace_value;
   std::string gem_family;
   std::string gem_item_id;
   std::string gem_icon_url;
   std::string new_attribute_id;
   std::string new_label;
   std::string new_value;
};

struct GemPlan {
   std::vector<GemChange> changes;
   std::int64_t projected_value = 0;
   std::int64_t value_uplift    = 0;
   std::int64_t socket_fee      = 0;
   std::int64_t net_uplift      = 0;
   std::string  confidence;
   std::int64_t sample_size     = 0;
};

struct GemOptimization {
   std::string            assumption;
   std::optional<GemPlan> one_socket;
   std::optional<GemPlan> two_socket;
   std::string            reason;
   std::string            note;
};

struct MarketAnalysis {
   std::int64_t       active_listings = 0;
   std::int64_t       sales_30d       = 0;
   std::optional<double> trend_percent;
   std::optional<std::int64_t> median_sale_seconds;
   std::optional<double>       days_supply;
   std::string                  price_stability;
   std::string        liquidity;
};

struct UtilityAnalysis {
   std::int64_t vendor_value       = 0;
   std::int64_t vendor_total       = 0;
   std::int64_t adventure_points   = 0;
   std::int64_t gear_score         = 0;
   std::int64_t max_stack_size     = 0;
   std::int64_t required_by_quests = 0;
   std::int64_t used_in_recipes    = 0;
   std::optional<std::int64_t> value_per_slot;
};

struct ValueDriver {
   std::string  attribute_id;
   std::string  label;
   std::int64_t gold_contribution = 0;
   std::string  basis;
};

struct SourceAnalysis {
   std::string           kind;
   std::string           heading;
   std::string           name;
   std::string           context;
   std::optional<double> drop_rate;
   std::optional<double> luck_drop_rate;
   std::optional<int>    luck;
};

struct TradeChatItem {
   std::string name;
   std::string rarity;
};

struct TradeChatMessage {
   std::string  message;
   std::string  observed_at;
   std::int64_t age_seconds = 0;
   std::vector<TradeChatItem> items;
};

struct TradeChatAnalysis {
   std::int64_t                 mentions_14d = 0;
   std::vector<TradeChatMessage> messages;
};

// Server-resolved item. Returned by lookup; parsing and localization
// happen server-side so the client stays language-agnostic.
struct TooltipLookup {
   // Client-side OCR text used for the diagnostic augment body. It is not
   // populated by or sent back to the API.
   std::string                    recognized_text;
   std::string                    item_id;
   std::string                    canonical_name;
   std::string                    display_name;
   std::string                    rarity;
   double                         match_confidence = 0.0;
   std::int64_t                   quantity = 1;
   bool                           tradeable = true;
   std::vector<AnalysisRoll>       rolls;
   std::vector<TooltipAttribute>  primary;
   std::vector<TooltipAttribute>  secondary;
   std::vector<TooltipAttribute>  details;
   Pricing                        pricing;
   std::optional<int>             roll_score;
   std::optional<int>             weighted_roll_score;
   std::optional<int>             relative_percentile;
   std::optional<ValueDriver>     value_driver;
   MarketAnalysis                 market_analysis;
   std::optional<SourceAnalysis>  source_analysis;
   TradeChatAnalysis              trade_chat;
   UtilityAnalysis                utility;
   GemOptimization                gem_optimization;
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

class DDBClient
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
   DDBClient  (Config cfg, gv::auth::Session* session, gv::db::Database* cache_db = nullptr);
   ~DDBClient ();

   DDBClient (const DDBClient&)            = delete;
   DDBClient& operator= (const DDBClient&) = delete;

   // POST /v2/grimvault/lookup. Sends the OCR text per contract §4.2, parses
   // the response per §4.3. Cached locally for `cache_ttl`.
   core::Result<TooltipLookup> lookup_tooltip (
      std::string_view raw_text,
      std::string_view language,
      std::chrono::seconds cache_ttl = std::chrono::seconds (300)
   );

   // POST /v2/grimvault/analyze. Resolves the complete OCR tooltip against
   // DDB, prices the exact rolls, and returns premium gem optimization.
   core::Result<TooltipLookup> analyze_tooltip (
      std::string_view raw_text,
      std::string_view language,
      float            confidence,
      std::chrono::seconds cache_ttl = std::chrono::seconds (180)
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

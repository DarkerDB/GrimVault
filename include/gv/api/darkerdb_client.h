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
   std::string           gem;
   std::string           gem_icon_url;
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
   int                    sockets         = 0;
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
   std::vector<GemPlan>    plans;
   // Parsed aliases for servers and callers that still use the original
   // fixed one/two-plan contract.
   std::optional<GemPlan> one_socket;
   std::optional<GemPlan> two_socket;
   std::string            reason;
   std::string            note;
};

struct ActivityCount {
   std::int64_t count = 0;
   bool         capped = false;
   std::int64_t window_hours = 0;
};

struct MarketAnalysis {
   ActivityCount active_listings;
   ActivityCount sales;
   // What the item actually sold for, unweighted. Distinct from
   // Pricing::median, which is adjusted for this instance's roll.
   std::optional<std::int64_t> average_sale_price;
   std::optional<std::int64_t> median_sale_price;
   std::optional<double> trend_percent;
   std::optional<std::int64_t> median_sale_seconds;
   std::optional<double>       days_supply;
   std::string                  price_stability;
   std::string        liquidity;
};

struct SimilarSaleRoll {
   std::string attribute_id;
   std::string label;
   std::string formatted_value;
};

// One sold copy from the bounded nearest-neighbour set used by valuation.
// `rolls` is additive to the original highlight pair so mixed client/server
// versions keep rendering during rollout.
struct SimilarSale {
   std::int64_t                price        = 0;
   std::int32_t                similarity   = 0;
   std::string                 sold_at;
   std::int64_t                age_seconds  = 0;
   std::optional<std::int64_t> sale_seconds;
   std::vector<SimilarSaleRoll> rolls;
   std::string                 highlight_label;
   std::string                 highlight_value;
};

// One quest that wants this item. `merchant_*` and the chain position are
// absent when the quest's chapter doesn't resolve to a merchant.
struct QuestUse {
   std::string                 merchant_id;
   std::string                 merchant_name;
   std::string                 merchant_icon_url;
   std::string                 quest_name;
   std::optional<std::int64_t> quest_index;
   std::optional<std::int64_t> quest_count;
   std::optional<std::int64_t> quantity;
};

// One material line in a recipe. `is_this` marks the hovered item.
struct RecipeItem {
   std::string  item_id;
   std::string  name;
   std::string  rarity;
   std::string  icon_url;
   std::int64_t quantity = 1;
   bool         is_this  = false;
};

// A recipe the hovered item is a material for.
struct RecipeUse {
   std::string             merchant_id;
   std::string             merchant_name;
   std::string             merchant_icon_url;
   std::optional<RecipeItem> output;
   std::vector<RecipeItem> materials;
};

struct UtilityAnalysis {
   std::int64_t vendor_value     = 0;
   std::int64_t vendor_total     = 0;
   std::int64_t adventure_points = 0;
   std::int64_t gear_score       = 0;
   std::int64_t max_stack_size   = 0;
   std::optional<std::int64_t> value_per_slot;
};

struct ValueDriver {
   std::string  attribute_id;
   std::string  label;
   std::int64_t gold_contribution = 0;
   std::string  basis;
};

struct SourceAlternative {
   std::string           id;
   std::string           icon_url;
   std::string           name;
   std::optional<double> drop_rate;
};

struct SourceAnalysis {
   std::string           kind;
   std::string           heading;
   std::string           id;
   std::string           icon_url;
   std::string           name;
   std::string           context;
   std::optional<double> drop_rate;
   std::optional<double> luck_drop_rate;
   std::optional<int>    luck;
   std::vector<SourceAlternative> alternates;
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

// A widget this player's plan does not grant, with the tier that would.
struct LockedWidget {
   std::string widget;
   std::string required_plan;
   std::string required_plan_name;
};

// The `entitlement` block every /v2/grimvault/analyze response carries.
// GrimVaultProjector has already withheld the blocks `granted` omits — this
// is the client's only way to tell "the plan does not include it" from
// "the analyzer had nothing to say", so the augment can upsell instead of
// silently rendering a gap.
struct Entitlement {
   std::string               plan;

   // The wire field is `slots`; that is a Qt keyword macro, and every UI
   // translation unit includes both this header and <QObject>.
   std::int64_t              slot_limit = 0;

   std::vector<std::string>  granted;
   std::vector<LockedWidget> locked;

   // Which tier each widget belongs to, and the tier order cheapest-first.
   // The card groups its sections by plan and cannot derive that from
   // `locked`, which is empty for a player who already owns everything.
   std::vector<std::pair<std::string, std::string>> tiers;
   std::vector<std::string>                         ladder;

   // Inline so the header-only augment payload builder (and its hermetic
   // unit test) need no link against gv::api.
   bool grants (std::string_view widget) const
   {
      // An absent entitlement block — an older server, or a fixture —
      // grants everything: GrimVaultProjector is the real boundary, and
      // withholding a block the server did send would only hide data the
      // caller paid for.
      if (plan.empty () && granted.empty ()) return true;

      for (const auto& allowed : granted) {
         if (allowed == widget) return true;
      }
      return false;
   }
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
   std::string                    artifact_type;
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
   std::vector<SimilarSale>        similar_sales;
   std::optional<SourceAnalysis>  source_analysis;
   TradeChatAnalysis              trade_chat;
   UtilityAnalysis                utility;
   std::vector<QuestUse>          quests;
   std::vector<RecipeUse>         recipes;
   GemOptimization                gem_optimization;
   Entitlement                    entitlement;
   std::string                    request_id;
   nlohmann::json                 raw;
};

// Dashboard-controlled settings snapshot from /v2/grimvault/settings.
//
// Wire schema is nested:
//
//    { "overlay":  { "mode", "alignment", "opacity", "scale", "offset_x", "offset_y",
//                    "is_indicator_visible" },
//      "tooltip":  { "sections": { "header", ... }, "analysis": { "market_value", ... },
//                    "is_price_history_sparkline_visible" },
//      "pricing":  { "currency_display" },
//      "behavior": { "is_auto_update_enabled", "is_launch_on_startup_enabled",
//                    "capture_fps", "capture_mode" },
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
      std::string  columns   = "auto";
      double       opacity   = 0.9;
      double       scale     = 1.0;
      std::int32_t offset_x  = 20;
      std::int32_t offset_y  = 20;

      // The bottom-right corner badge, not the augment card — a player who
      // sets mode=disabled keeps it, so it carries its own toggle.
      bool is_indicator_visible = true;
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

      // tooltip.analysis.* — the per-widget visibility toggles that drive
      // the augment's `visible_sections`. Deliberately NOT a fixed struct:
      // grimvault-widgets.yaml is the single source of the vocabulary, so a
      // widget added server-side has to reach the card without a client
      // release. Order is the server's render order; an absent slug means
      // "shown", matching the tooltip library's default.
      std::vector<std::pair<std::string, bool>> analysis;

      bool shows (std::string_view widget) const
      {
         for (const auto& [slug, visible] : analysis) {
            if (slug == widget) return visible;
         }
         // Unknown slug -> shown, matching the tooltip library's
         // normalizeVisible default.
         return true;
      }
   };

   struct Pricing {
      // absolute | compact. Matches pricing:currency_display's allowlist in
      // data/darkerdb/grimvault-settings.yaml.
      std::string currency_display = "absolute";
   };

   struct Behavior {
      bool is_auto_update_enabled       = true;
      bool is_launch_on_startup_enabled = true;
      bool is_performance_mode_enabled  = false;
      std::int32_t capture_fps          = 15;
      std::string capture_mode          = "automatic";
      std::string language              = "automatic";
   };

   struct Hotkeys {
      std::string toggle_overlay  = "Ctrl+Shift+G";
      std::string force_refresh   = "F5";
      std::string open_in_browser = "Ctrl+Shift+D";
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

core::Result<SettingsBundle> parse_settings (std::string_view json);

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

   // POST /v2/grimvault/lookup. Personalized responses are partitioned by
   // account and cached briefly so repeated hovers do not repeat a round trip.
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
      std::string_view capture_backend,
      const std::unordered_map<std::string, std::string>& gems = {},
      const std::vector<std::string>& enabled_widgets = {},
      std::chrono::seconds cache_ttl = std::chrono::seconds (180)
   );

   // POST /v2/grimvault/ping. Auth probe; no quota consumption per §4.7.
   core::Result<PingResult> ping ();

   // GET /v2/grimvault/settings. Returns the dashboard-controlled settings
   // bundle. Source of truth for any key the server manages.
   core::Result<SettingsBundle> get_settings ();

   /// Abort current transfers. Safe to call from another thread and reusable;
   /// future requests are assigned a new cancellation generation.
   void cancel_pending () noexcept;

   /// Abort only the latency-critical tooltip lane. Settings and session work
   /// continue while a newly hovered item supersedes an older analysis.
   void cancel_analysis () noexcept;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::api

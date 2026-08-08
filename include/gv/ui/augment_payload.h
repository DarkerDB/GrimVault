#pragma once

#include <gv/api/darkerdb_client.h>

#include <nlohmann/json.hpp>

#include <cstdint>

namespace gv::ui::augment {

// TooltipLookup -> ddb-tooltips augment payload. Header-only so unit tests
// can exercise the mapping without linking the WebView2/Qt UI stack.
//
//    lookup { canonical_name:"Ruby Silver Ring", rarity:"rare",
//             primary:2, secondary:3, pricing.median:412 }
//      -> { name:"<GrimVault>", rarity:"rare",
//           sections:[ text, stats, stats:secondary, rows, pricing ] }

inline nlohmann::json stats_section (const char* variant,
                                     const std::vector<gv::api::TooltipAttribute>& attrs)
{
   nlohmann::json entries = nlohmann::json::array ();
   for (const auto& a : attrs) {
      entries.push_back ({ { "label", a.label }, { "value", a.value } });
   }
   return { { "kind", "stats" }, { "variant", variant }, { "entries", std::move (entries) } };
}

inline nlohmann::json entity (const gv::api::TooltipLookup& lookup)
{
   nlohmann::json sections = nlohmann::json::array ();

   // The item itself leads the body; the card title is the fixed Augment
   // identity.
   sections.push_back ({ { "kind", "text" }, { "title", lookup.canonical_name } });

   if (!lookup.primary.empty ()) {
      sections.push_back (stats_section ("primary", lookup.primary));
   }
   if (!lookup.secondary.empty ()) {
      sections.push_back (stats_section ("secondary", lookup.secondary));
   }

   if (!lookup.details.empty ()) {
      nlohmann::json rows = nlohmann::json::array ();
      for (const auto& d : lookup.details) {
         rows.push_back ({ { "label", d.label }, { "value", d.value } });
      }
      sections.push_back ({ { "kind", "rows" }, { "rows", std::move (rows) } });
   }

   if (lookup.pricing.median > 0 || lookup.pricing.low > 0) {
      sections.push_back ({
         { "kind",   "pricing" },
         { "market", lookup.pricing.median },
         { "vendor", lookup.pricing.low },
      });
   }

   return {
      { "name",     "<GrimVault>" },
      { "rarity",   lookup.rarity.empty () ? "common" : lookup.rarity },
      { "sections", std::move (sections) },
   };
}

inline nlohmann::json render_message (const gv::api::TooltipLookup& lookup,
                                      std::uint64_t seq)
{
   return {
      { "type",   "render" },
      { "seq",    seq },
      { "entity", entity (lookup) },
      { "params", { { "kind", "augment" }, { "compact", true } } },
   };
}

} // namespace gv::ui::augment

#include <gv/api/darkerdb_client.h>
#include <gv/api/price_check_url.h>

#include <QUrl>
#include <QUrlQuery>
#include <QStringList>

#include <charconv>
#include <cmath>

namespace gv::api {

std::string price_check_path (const TooltipLookup& lookup)
{
   if (lookup.item_id.empty ()) {
      return {};
   }

   QUrl url { "/tools/price-check" };
   QUrlQuery query;
   query.addQueryItem ("item", QString::fromStdString (lookup.item_id));
   QStringList order;
   QStringList gems;

   for (const auto& roll : lookup.rolls) {
      const bool supported_slot = roll.slot == "primary" || roll.slot == "secondary";

      if (!supported_slot || roll.attribute_id.empty () || !std::isfinite (roll.value)) {
         continue;
      }

      char number[64];
      const auto formatted = std::to_chars (number, number + sizeof (number), roll.value);

      if (formatted.ec != std::errc {}) {
         continue;
      }

      const auto attribute = QString::fromStdString (roll.attribute_id);
      const auto key = QString::fromStdString (roll.slot) + "[" + attribute + "]";
      query.addQueryItem (key, QString::fromLatin1 (number, formatted.ptr - number));

      if (roll.slot == "secondary") {
         order.append (attribute);

         if (!roll.gem.empty ()) {
            gems.append (attribute + ":" + QString::fromStdString (roll.gem));
         }
      }
   }

   if (!order.isEmpty ()) {
      query.addQueryItem ("order", order.join (","));
   }

   if (!gems.isEmpty ()) {
      query.addQueryItem ("gems", gems.join (","));
   }

   url.setQuery (query);
   return url.toString (QUrl::FullyEncoded).toStdString ();
}

}

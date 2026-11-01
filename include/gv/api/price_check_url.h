#pragma once

#include <string>

namespace gv::api {

struct TooltipLookup;

// Build the Price Check share route from an analyzed item and its native roll
// values. Example: item id.item.bandage_1001 without rolls produces
// /tools/price-check?item=id.item.bandage_1001. An empty item ID produces "".
std::string price_check_path (const TooltipLookup& lookup);

}

#pragma once

#include <gv/api/darkerdb_client.h>

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gv::api::response {

const nlohmann::json& body (const nlohmann::json& json);
TooltipLookup lookup (nlohmann::json json);
TooltipLookup analysis (nlohmann::json json);
PingResult ping (nlohmann::json json);
void settings (const nlohmann::json& body, SettingsBundle& settings,
               const std::vector<std::string>& analysis_order);

}

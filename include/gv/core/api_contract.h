#pragma once

#include <string_view>

namespace gv::core::api_contract {

inline constexpr std::string_view header_name = "X-API-Version";
inline constexpr std::string_view version = "2026-08-15";
inline constexpr std::string_view header_line = "X-API-Version: 2026-08-15";

} // namespace gv::core::api_contract

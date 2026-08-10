#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace gv::capture {

// Capture-backend policy, cloud-synced as behavior:capture_mode. Automatic
// follows the platform-preferred strategy ladder with runtime failover; the
// Force values pin one backend and disable failover.
enum class CaptureMode : std::uint8_t { Automatic, ForceWgc, ForceDxgi, ForceGdi };

// Wire value for a mode ("automatic", "wgc", "dxgi", "gdi").
constexpr std::string_view capture_mode_name (CaptureMode mode) noexcept
{
   switch (mode) {
      case CaptureMode::ForceWgc:  return "wgc";
      case CaptureMode::ForceDxgi: return "dxgi";
      case CaptureMode::ForceGdi:  return "gdi";
      default:                     return "automatic";
   }
}

// Inverse of capture_mode_name. Empty for an unrecognized value so callers
// keep their own default.
constexpr std::optional<CaptureMode> parse_capture_mode (std::string_view value) noexcept
{
   if (value == "automatic") return CaptureMode::Automatic;
   if (value == "wgc")       return CaptureMode::ForceWgc;
   if (value == "dxgi")      return CaptureMode::ForceDxgi;
   if (value == "gdi")       return CaptureMode::ForceGdi;
   return std::nullopt;
}

} // namespace gv::capture

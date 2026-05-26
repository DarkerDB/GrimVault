#pragma once

#include <gv/core/result.h>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gv::db { class UserSettingsRepo; }

namespace gv::core {

enum class Mode      { Automatic, Manual, Disabled };
enum class Alignment { Attached, TopLeft, TopRight, BottomLeft, BottomRight };

class Settings
{
public:
   explicit Settings (gv::db::UserSettingsRepo& repo);

   bool                     telemetry          ();
   bool                     auto_updates       ();
   bool                     launch_on_startup  ();
   Mode                     default_mode       ();
   Alignment                alignment          ();
   std::vector<std::string> components         ();
   double                   scale              ();

   Result<void> set_telemetry          (bool                            v);
   Result<void> set_auto_updates       (bool                            v);
   Result<void> set_launch_on_startup  (bool                            v);
   Result<void> set_default_mode       (Mode                            v);
   Result<void> set_alignment          (Alignment                       v);
   Result<void> set_components         (std::span<const std::string>    v);
   Result<void> set_scale              (double                          v);

   static std::string_view to_string (Mode);
   static std::string_view to_string (Alignment);

   static Result<Mode>      parse_mode      (std::string_view);
   static Result<Alignment> parse_alignment (std::string_view);

private:
   gv::db::UserSettingsRepo& repo_;
};

} // namespace gv::core

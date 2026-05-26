#pragma once

#include <gv/core/result.h>

#include <string_view>

namespace gv::core {

// Manages the HKCU\Software\Microsoft\Windows\CurrentVersion\Run entry that
// makes Windows launch the app at user sign-in.
class StartupLink
{
public:
   // Create/update the Run entry to launch `exe_path` with optional `args`
   // at sign-in.
   static Result<void> enable (std::string_view app_name,
                               std::string_view exe_path,
                               std::string_view args = {});

   // Remove the Run entry. Returns ok() even if it didn't exist.
   static Result<void> disable (std::string_view app_name);

   // True if the Run entry is currently present.
   static Result<bool> is_enabled (std::string_view app_name);
};

} // namespace gv::core

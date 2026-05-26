#pragma once

#include <gv/core/result.h>

#include <filesystem>

namespace gv::db {
   class UserSettingsRepo;
   class UserHotkeysRepo;
}

namespace gv::core {

class IniMigrator
{
public:
   // One-shot: reads `ini_path` if it exists, writes rows into the two repos,
   // then renames the INI to <path>.migrated to mark it consumed.
   //
   // Idempotent: if user_settings already has rows, returns immediately
   // without re-reading the INI (a previous run already migrated).
   //
   // Defaults are written first (so a fresh install with no INI is also
   // initialized); the INI overlay only updates the keys it carries.
   static Result<bool> run (
      const std::filesystem::path& ini_path,
      gv::db::UserSettingsRepo&    settings,
      gv::db::UserHotkeysRepo&     hotkeys
   );
};

} // namespace gv::core

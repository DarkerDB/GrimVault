#include <gv/core/ini_migrator.h>
#include <gv/core/logger.h>
#include <gv/db/repos/user_hotkeys_repo.h>
#include <gv/db/repos/user_settings_repo.h>

#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gv::core {

namespace {

   struct IniEntry {
      std::string section;
      std::string key;
      std::string value;
   };

   std::string_view trim (std::string_view s)
   {
      while (!s.empty () && (s.front () == ' ' || s.front () == '\t' || s.front () == '\r')) s.remove_prefix (1);
      while (!s.empty () && (s.back  () == ' ' || s.back  () == '\t' || s.back  () == '\r')) s.remove_suffix (1);
      return s;
   }

   std::vector<IniEntry> parse_ini (const std::filesystem::path& path)
   {
      std::vector<IniEntry> out;
      std::ifstream         in { path };

      if (!in) {
         return out;
      }

      std::string section;
      std::string line;

      while (std::getline (in, line)) {
         auto sv = trim (line);

         if (sv.empty () || sv.front () == ';' || sv.front () == '#') continue;

         if (sv.front () == '[' && sv.back () == ']') {
            section = std::string { trim (sv.substr (1, sv.size () - 2)) };
            continue;
         }

         auto eq = sv.find ('=');

         if (eq == std::string_view::npos) continue;

         auto key   = trim (sv.substr (0, eq));
         auto value = trim (sv.substr (eq + 1));

         if (key.empty ()) continue;

         out.emplace_back (IniEntry {
            .section = section,
            .key     = std::string { key },
            .value   = std::string { value },
         });
      }

      return out;
   }

   constexpr struct { std::string_view key; std::string_view value; } k_defaults [] = {
      { "general:telemetry",          "true" },
      { "general:auto_updates",       "true" },
      { "general:launch_on_startup",  "true" },
      { "general:default_mode",       "automatic" },
      { "general:alignment",          "attached" },
      { "general:components",         "header,primary,secondary,details,quests,pricing" },
      { "general:scale",              "1.0" },
   };

   constexpr struct { std::string_view action_id; std::string_view accelerator; } k_default_hotkeys [] = {
      { "scan_now",      "F5" },
      { "toggle_mode",   "F6" },
      { "debug_toggle",  "F7" },
      { "clear_overlay", "F8" },
   };

} // namespace

Result<bool> IniMigrator::run (
   const std::filesystem::path& ini_path,
   gv::db::UserSettingsRepo&    settings,
   gv::db::UserHotkeysRepo&     hotkeys
) {
   auto existing = settings.all ();

   if (!existing.has_value ()) {
      return fail (existing.error ());
   }

   if (!existing->empty ()) {
      return false;
   }

   for (const auto& d : k_defaults) {
      auto r = settings.set (d.key, d.value);
      if (!r.has_value ()) return fail (r.error ());
   }

   for (const auto& h : k_default_hotkeys) {
      auto r = hotkeys.set (h.action_id, h.accelerator);
      if (!r.has_value ()) return fail (r.error ());
   }

   std::error_code ec;

   if (!std::filesystem::exists (ini_path, ec)) {
      Logger::info ("ini_migrator: no INI at {}; defaults written", ini_path.string ());
      return true;
   }

   const auto entries = parse_ini (ini_path);

   for (const auto& e : entries) {
      if (e.section == "general") {
         auto r = settings.set ("general:" + e.key, e.value);
         if (!r.has_value ()) return fail (r.error ());
      } else if (e.section == "hotkeys") {
         auto r = hotkeys.set (e.key, e.value);
         if (!r.has_value ()) return fail (r.error ());
      } else {
         Logger::warn ("ini_migrator: ignoring unknown section [{}] key '{}'", e.section, e.key);
      }
   }

   std::filesystem::path migrated = ini_path;
   migrated += ".migrated";

   std::filesystem::rename (ini_path, migrated, ec);

   if (ec) {
      Logger::warn ("ini_migrator: failed to rename {} → {}: {}",
         ini_path.string (), migrated.string (), ec.message ());
   } else {
      Logger::info ("ini_migrator: migrated INI; renamed to {}", migrated.string ());
   }

   return true;
}

} // namespace gv::core

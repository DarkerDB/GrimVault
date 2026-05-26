#include <gv/db/repos/user_settings_repo.h>

#include <SQLiteCpp/SQLiteCpp.h>

namespace gv::db {

UserSettingsRepo::UserSettingsRepo (Database& db) : db_ (db) {}

core::Result<void> UserSettingsRepo::set (std::string_view key, std::string_view value)
{
   try {
      SQLite::Statement stmt { db_.sqlite (), R"sql(
         INSERT INTO user_settings (key, value, updated_at)
              VALUES               (?,   ?,     unixepoch ())
         ON CONFLICT (key) DO UPDATE
            SET value      = excluded.value,
                updated_at = excluded.updated_at
      )sql" };

      stmt.bind (1, std::string { key });
      stmt.bind (2, std::string { value });
      stmt.exec ();
      return {};
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "user_settings.set({}) failed: {}", key, e.what ()
      ));
   }
}

core::Result<std::optional<std::string>> UserSettingsRepo::get (std::string_view key)
{
   try {
      SQLite::Statement stmt { db_.sqlite (),
         "SELECT value FROM user_settings WHERE key = ?"
      };

      stmt.bind (1, std::string { key });

      if (stmt.executeStep ()) {
         return std::optional<std::string> { stmt.getColumn (0).getString () };
      }

      return std::optional<std::string> {};
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "user_settings.get({}) failed: {}", key, e.what ()
      ));
   }
}

core::Result<std::unordered_map<std::string, std::string>> UserSettingsRepo::all ()
{
   try {
      std::unordered_map<std::string, std::string> out;

      SQLite::Statement stmt { db_.sqlite (),
         "SELECT key, value FROM user_settings"
      };

      while (stmt.executeStep ()) {
         out.emplace (
            stmt.getColumn (0).getString (),
            stmt.getColumn (1).getString ()
         );
      }

      return out;
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "user_settings.all() failed: {}", e.what ()
      ));
   }
}

core::Result<void> UserSettingsRepo::erase (std::string_view key)
{
   try {
      SQLite::Statement stmt { db_.sqlite (),
         "DELETE FROM user_settings WHERE key = ?"
      };

      stmt.bind (1, std::string { key });
      stmt.exec ();
      return {};
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "user_settings.erase({}) failed: {}", key, e.what ()
      ));
   }
}

} // namespace gv::db

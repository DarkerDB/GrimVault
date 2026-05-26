#include <gv/db/repos/user_hotkeys_repo.h>

#include <SQLiteCpp/SQLiteCpp.h>

namespace gv::db {

UserHotkeysRepo::UserHotkeysRepo (Database& db) : db_ (db) {}

core::Result<void> UserHotkeysRepo::set (std::string_view action_id, std::string_view accelerator)
{
   try {
      SQLite::Statement stmt { db_.sqlite (), R"sql(
         INSERT INTO user_hotkeys (action_id, accelerator)
              VALUES              (?,         ?)
         ON CONFLICT (action_id) DO UPDATE
            SET accelerator = excluded.accelerator
      )sql" };

      stmt.bind (1, std::string { action_id });
      stmt.bind (2, std::string { accelerator });
      stmt.exec ();
      return {};
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "user_hotkeys.set({}) failed: {}", action_id, e.what ()
      ));
   }
}

core::Result<std::optional<std::string>> UserHotkeysRepo::get (std::string_view action_id)
{
   try {
      SQLite::Statement stmt { db_.sqlite (),
         "SELECT accelerator FROM user_hotkeys WHERE action_id = ?"
      };

      stmt.bind (1, std::string { action_id });

      if (stmt.executeStep ()) {
         return std::optional<std::string> { stmt.getColumn (0).getString () };
      }

      return std::optional<std::string> {};
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "user_hotkeys.get({}) failed: {}", action_id, e.what ()
      ));
   }
}

core::Result<std::unordered_map<std::string, std::string>> UserHotkeysRepo::all ()
{
   try {
      std::unordered_map<std::string, std::string> out;

      SQLite::Statement stmt { db_.sqlite (),
         "SELECT action_id, accelerator FROM user_hotkeys"
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
         "user_hotkeys.all() failed: {}", e.what ()
      ));
   }
}

} // namespace gv::db

#include <gv/db/database.h>
#include <gv/core/logger.h>

#include "migrations.h"

#include <SQLiteCpp/SQLiteCpp.h>

#include <algorithm>
#include <utility>

namespace gv::db {

struct Database::Impl
{
   std::filesystem::path             path;
   std::unique_ptr<SQLite::Database> conn;
};

Database::Database (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}
Database::~Database ()                                                = default;
Database::Database (Database&&)                       noexcept       = default;
Database& Database::operator= (Database&&)             noexcept       = default;

core::Result<std::unique_ptr<Database>> Database::open (const std::filesystem::path& path)
{
   try {
      std::error_code ec;
      std::filesystem::create_directories (path.parent_path (), ec);

      auto impl  = std::make_unique<Impl> ();
      impl->path = path;
      impl->conn = std::make_unique<SQLite::Database> (
         path.string (),
         SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE
      );

      impl->conn->exec ("PRAGMA foreign_keys  = ON");
      impl->conn->exec ("PRAGMA journal_mode  = WAL");
      impl->conn->exec ("PRAGMA synchronous   = NORMAL");
      impl->conn->exec ("PRAGMA temp_store    = MEMORY");

      auto db = std::unique_ptr<Database> (new Database (std::move (impl)));

      auto migrated = db->migrate ();

      if (!migrated.has_value ()) {
         return core::fail (migrated.error ());
      }

      core::Logger::info ("db: opened {} (user_version={})",
         path.string (), *migrated);

      return db;
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "Failed to open database at {}: {}",
         path.string (),
         e.what ()
      ));
   }
}

core::Result<int> Database::migrate ()
{
   try {
      const int current = impl_->conn->execAndGet ("PRAGMA user_version").getInt ();

      SQLite::Transaction tx { *impl_->conn };

      for (const auto& m : migrations) {
         if (m.version <= current) continue;

         core::Logger::info ("db: applying migration {}", m.version);

         impl_->conn->exec (std::string { m.sql });

         impl_->conn->exec (
            "PRAGMA user_version = " + std::to_string (m.version)
         );
      }

      tx.commit ();

      const int after = impl_->conn->execAndGet ("PRAGMA user_version").getInt ();

      return after;
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "Migration failed: {}",
         e.what ()
      ));
   }
}

core::Result<void> Database::exec (std::string_view sql)
{
   try {
      impl_->conn->exec (std::string { sql });
      return {};
   } catch (const std::exception& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Database,
         "exec failed: {}",
         e.what ()
      ));
   }
}

SQLite::Database& Database::sqlite () noexcept
{
   return *impl_->conn;
}

const std::filesystem::path& Database::path () const noexcept
{
   return impl_->path;
}

} // namespace gv::db

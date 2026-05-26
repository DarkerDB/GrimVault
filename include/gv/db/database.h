#pragma once

#include <gv/core/result.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string_view>

namespace SQLite { class Database; class Transaction; }

namespace gv::db {

// Single SQLite connection wrapper. Owns the file handle, applies pending
// migrations on open, and exposes the underlying SQLite::Database to
// repositories (which build their own prepared statements).
//
// Thread model: one Database per logical owner. SQLite itself is serialized
// (default mode); concurrent writers should go through a single Database
// instance on a dedicated thread.
class Database
{
public:
   ~Database ();

   Database (const Database&)            = delete;
   Database& operator= (const Database&) = delete;
   Database (Database&&)                 noexcept;
   Database& operator= (Database&&)      noexcept;

   // Open (or create) the database file at `path`. Applies pending migrations
   // before returning. Sets sensible pragmas (WAL, foreign_keys=ON, synchronous=NORMAL).
   static core::Result<std::unique_ptr<Database>> open (const std::filesystem::path& path);

   // Run any embedded migrations whose number is greater than the current
   // `PRAGMA user_version`. Returns the resulting user_version.
   core::Result<int> migrate ();

   // Execute one or more statements (no params, no result rows).
   core::Result<void> exec (std::string_view sql);

   // Access for repositories. Lifetime is tied to this Database.
   SQLite::Database& sqlite () noexcept;

   // The file path this Database is bound to.
   const std::filesystem::path& path () const noexcept;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;

   explicit Database (std::unique_ptr<Impl> impl);
};

} // namespace gv::db

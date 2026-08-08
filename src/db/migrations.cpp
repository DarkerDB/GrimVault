#include "migrations.h"

#include <array>
#include <span>

namespace gv::db {

namespace {

   // Embedded at configure time. The raw string below is replaced by CMake
   // via configure_file when generating this translation unit's companion
   // header. For now, the SQL is inlined here directly so the module is
   // self-contained — keeping migrations versioned by file under db/migrations/
   // and mirrored here is the SSOT trade.
   constexpr std::string_view k_migration_0001 =
#include "migrations/0001_init.sql.inc"
   ;

   constexpr std::string_view k_migration_0002 =
#include "migrations/0002_account_sessions.sql.inc"
   ;

   constexpr std::array<Migration, 2> k_table {{
      { 1, k_migration_0001 },
      { 2, k_migration_0002 },
   }};

} // namespace

const std::span<const Migration> migrations { k_table };

} // namespace gv::db

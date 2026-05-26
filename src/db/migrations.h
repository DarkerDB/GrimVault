#pragma once

#include <span>
#include <string_view>

namespace gv::db {

// One entry per numbered migration. Add new entries here as schema evolves;
// the Database migrator applies any whose version is greater than the
// current PRAGMA user_version.
struct Migration {
   int              version;
   std::string_view sql;
};

// Embedded at build time via configure_file → migrations_data.h.
extern const std::span<const Migration> migrations;

} // namespace gv::db

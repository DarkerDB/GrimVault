#pragma once

#include <gv/core/result.h>
#include <gv/db/database.h>

#include <optional>
#include <string>
#include <unordered_map>

namespace gv::db {

class UserSettingsRepo
{
public:
   explicit UserSettingsRepo (Database& db);

   core::Result<void>                  set (std::string_view key, std::string_view value);
   core::Result<std::optional<std::string>> get (std::string_view key);
   core::Result<std::unordered_map<std::string, std::string>> all ();
   core::Result<void>                  erase (std::string_view key);

private:
   Database& db_;
};

} // namespace gv::db

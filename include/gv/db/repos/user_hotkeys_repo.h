#pragma once

#include <gv/core/result.h>
#include <gv/db/database.h>

#include <optional>
#include <string>
#include <unordered_map>

namespace gv::db {

class UserHotkeysRepo
{
public:
   explicit UserHotkeysRepo (Database& db);

   core::Result<void> set (std::string_view action_id, std::string_view accelerator);
   core::Result<std::optional<std::string>> get (std::string_view action_id);
   core::Result<std::unordered_map<std::string, std::string>> all ();

private:
   Database& db_;
};

} // namespace gv::db

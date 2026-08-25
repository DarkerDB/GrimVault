#pragma once

#include <filesystem>
#include <string_view>

namespace gv::collection {

class Collector;

bool submit_latest_log (
   Collector& collector,
   const std::filesystem::path& directory,
   std::string_view install_id,
   std::string_view version,
   std::string_view environment
);

}

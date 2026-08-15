#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace gv::core::diagnostics {

const std::string& session_id ();
const std::string& install_id (const std::filesystem::path& data_dir);

std::vector<std::string> machine ();

std::string process_sample ();

} // namespace gv::core::diagnostics

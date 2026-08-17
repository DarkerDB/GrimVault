#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace gv::ocr {

std::optional<std::string> canonical_locale (std::string_view value);
std::optional<std::string> read_game_locale (const std::filesystem::path& path);
std::optional<std::string> detect_game_locale ();

}

#include <gv/ocr/game_locale.h>

#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>

namespace gv::ocr {

namespace {

   bool equal_folded (std::string_view left, std::string_view right)
   {
      if (left.size () != right.size ()) return false;
      for (std::size_t i = 0; i < left.size (); ++i) {
         const auto l = static_cast<unsigned char> (left [i]);
         const auto r = static_cast<unsigned char> (right [i]);
         if (std::tolower (l) != std::tolower (r)) return false;
      }
      return true;
   }

   std::optional<std::filesystem::path> local_app_data ()
   {
#ifdef _WIN32
      wchar_t* value = nullptr;
      std::size_t size = 0;
      if (_wdupenv_s (&value, &size, L"LOCALAPPDATA") != 0 || !value) return std::nullopt;
      std::filesystem::path path { value };
      std::free (value);
      return path;
#else
      const auto* value = std::getenv ("LOCALAPPDATA");
      if (!value || !*value) return std::nullopt;
      return std::filesystem::path { value };
#endif
   }

}

std::optional<std::string> canonical_locale (std::string_view value)
{
   constexpr std::array locales {
      std::string_view { "de" },
      std::string_view { "en" },
      std::string_view { "es" },
      std::string_view { "fr" },
      std::string_view { "ja" },
      std::string_view { "ko" },
      std::string_view { "pt-BR" },
      std::string_view { "ru" },
      std::string_view { "zh-Hans" },
      std::string_view { "zh-Hant" },
   };

   for (const auto locale : locales) {
      if (equal_folded (value, locale)) return std::string { locale };
   }
   return std::nullopt;
}

std::optional<std::string> read_game_locale (const std::filesystem::path& path)
{
   std::ifstream input { path };
   if (!input) return std::nullopt;

   const std::string contents {
      std::istreambuf_iterator<char> { input },
      std::istreambuf_iterator<char> {}
   };
   static const std::regex culture { R"locale(\bculture\s*=\s*"([^"]+)")locale",
      std::regex::icase };
   std::smatch match;
   if (!std::regex_search (contents, match, culture)) return std::nullopt;
   return canonical_locale (match [1].str ());
}

std::optional<std::string> detect_game_locale ()
{
   const auto root = local_app_data ();
   if (!root) return std::nullopt;
   return read_game_locale (*root
      / "DungeonCrawler" / "Saved" / "Config" / "Windows" / "GameUserSettings.ini");
}

}

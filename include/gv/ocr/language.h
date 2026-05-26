#pragma once

#include <string_view>

namespace gv::ocr {

// PaddleOCR ships per-language-family recognizer models. The detector model
// (DB) is language-agnostic so we load it once.
enum class LanguageFamily {
   Latin,        // en, de, es, fr, pt-BR, ru
   Japanese,     // ja
   Korean,       // ko
   ChineseSimp,  // zh-Hans
   ChineseTrad,  // zh-Hant
};

constexpr std::string_view family_dir (LanguageFamily f) noexcept
{
   switch (f) {
      case LanguageFamily::Latin:        return "latin";
      case LanguageFamily::Japanese:     return "japan";
      case LanguageFamily::Korean:       return "korean";
      case LanguageFamily::ChineseSimp:  return "ch";
      case LanguageFamily::ChineseTrad:  return "chinese_cht";
   }
   return "latin";
}

// Map a game locale code (matching i18n/<lang>/ directories) to the family
// whose Paddle recognizer should be loaded.
constexpr LanguageFamily family_of (std::string_view game_locale) noexcept
{
   if (game_locale == "ja")      return LanguageFamily::Japanese;
   if (game_locale == "ko")      return LanguageFamily::Korean;
   if (game_locale == "zh-Hans") return LanguageFamily::ChineseSimp;
   if (game_locale == "zh-Hant") return LanguageFamily::ChineseTrad;
   return LanguageFamily::Latin;
}

} // namespace gv::ocr

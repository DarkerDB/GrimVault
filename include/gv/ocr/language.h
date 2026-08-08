#pragma once

#include <string_view>

namespace gv::ocr {

// PaddleOCR ships per-language-family recognizer models. The detector model
// (DB) is language-agnostic so we load it once.
//
// PP-OCRv5 has no separate japan/chinese_cht models: the base "ch" model
// covers Simplified + Traditional Chinese and Japanese. Latin does not
// cover Cyrillic; Russian needs the eslav (East Slavic) model.
enum class LanguageFamily {
   English,  // en (narrow alphabet; higher English accuracy)
   Latin,    // de, es, fr, pt-BR
   Eslav,    // ru
   Korean,   // ko
   Chinese,  // ja, zh-Hans, zh-Hant
};

constexpr std::string_view family_dir (LanguageFamily f) noexcept
{
   switch (f) {
      case LanguageFamily::English: return "en";
      case LanguageFamily::Latin:   return "latin";
      case LanguageFamily::Eslav:   return "eslav";
      case LanguageFamily::Korean:  return "korean";
      case LanguageFamily::Chinese: return "ch";
   }
   return "latin";
}

// Map a game locale code (matching i18n/<lang>/ directories) to the family
// whose Paddle recognizer should be loaded.
constexpr LanguageFamily family_of (std::string_view game_locale) noexcept
{
   if (game_locale == "en")      return LanguageFamily::English;
   if (game_locale == "ru")      return LanguageFamily::Eslav;
   if (game_locale == "ko")      return LanguageFamily::Korean;
   if (game_locale == "ja")      return LanguageFamily::Chinese;
   if (game_locale == "zh-Hans") return LanguageFamily::Chinese;
   if (game_locale == "zh-Hant") return LanguageFamily::Chinese;
   return LanguageFamily::Latin;
}

} // namespace gv::ocr
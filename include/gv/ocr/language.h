#pragma once

#include <string_view>

namespace gv::ocr {

// Recognizer filenames under <models_root>/paddle/<family>/.
//
// Each name carries what distinguishes one recognizer from another: who
// trained it and the line geometry it expects. `ppocr` is stock PaddleOCR,
// whose character set is the family dictionary; `tooltip` is ours, trained
// against the game's own faces on a generated current-game character set.
//
// A dictionary is named for the model it belongs to, not the directory, so a
// family holding two charsets cannot mispair them. Body and title share one
// because they share a charset — the split is line role, not vocabulary.
namespace model_files {

   inline constexpr const char* rec_ppocr_narrow      = "rec-ppocr-48x320.onnx";
   inline constexpr const char* rec_ppocr_narrow_dict = "rec-ppocr-48x320.dict.txt";
   inline constexpr const char* rec_ppocr_wide        = "rec-ppocr-48x960.onnx";
   inline constexpr const char* rec_ppocr_wide_dict   = "rec-ppocr-48x960.dict.txt";
   inline constexpr const char* rec_tooltip_body      = "rec-tooltip-body-48x960.onnx";
   inline constexpr const char* rec_tooltip_title     = "rec-tooltip-title-48x960.onnx";
   inline constexpr const char* rec_tooltip_dict      = "rec-tooltip-48x960.dict.txt";

} // namespace model_files

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

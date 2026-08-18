#include <gv/ocr/language.h>

#include <gtest/gtest.h>

using gv::ocr::LanguageFamily;
using gv::ocr::family_dir;
using gv::ocr::family_of;

TEST (Language, ActiveLocaleUsesEnglishRecognizer)
{
   EXPECT_EQ (gv::ocr::active_locale, "en");
   EXPECT_EQ (family_of (gv::ocr::active_locale), LanguageFamily::English);
}

TEST (Language, AllTenLocalesResolve)
{
   EXPECT_EQ (family_of ("de"),      LanguageFamily::Latin);
   EXPECT_EQ (family_of ("en"),      LanguageFamily::English);
   EXPECT_EQ (family_of ("es"),      LanguageFamily::Latin);
   EXPECT_EQ (family_of ("fr"),      LanguageFamily::Latin);
   EXPECT_EQ (family_of ("pt-BR"),   LanguageFamily::Latin);
   EXPECT_EQ (family_of ("ru"),      LanguageFamily::Eslav);
   EXPECT_EQ (family_of ("ko"),      LanguageFamily::Korean);
   EXPECT_EQ (family_of ("ja"),      LanguageFamily::Chinese);
   EXPECT_EQ (family_of ("zh-Hans"), LanguageFamily::Chinese);
   EXPECT_EQ (family_of ("zh-Hant"), LanguageFamily::Chinese);
}

TEST (Language, UnknownLocaleFallsBackToLatin)
{
   EXPECT_EQ (family_of (""),   LanguageFamily::Latin);
   EXPECT_EQ (family_of ("xx"), LanguageFamily::Latin);
}

TEST (Language, FamilyDirsMatchModelLayout)
{
   EXPECT_EQ (family_dir (LanguageFamily::English), "en");
   EXPECT_EQ (family_dir (LanguageFamily::Latin),   "latin");
   EXPECT_EQ (family_dir (LanguageFamily::Eslav),   "eslav");
   EXPECT_EQ (family_dir (LanguageFamily::Korean),  "korean");
   EXPECT_EQ (family_dir (LanguageFamily::Chinese), "ch");
}

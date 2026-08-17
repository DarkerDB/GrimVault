#include <gv/ocr/game_locale.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

   class GameLocaleTest : public testing::Test
   {
   protected:
      void SetUp () override
      {
         path = std::filesystem::temp_directory_path ()
            / ("grimvault-game-locale-" + std::to_string (
               testing::UnitTest::GetInstance ()->random_seed ()) + ".ini");
      }

      void TearDown () override
      {
         std::error_code error;
         std::filesystem::remove (path, error);
      }

      std::filesystem::path path;
   };

}

TEST (GameLocale, CanonicalizesSupportedLocales)
{
   EXPECT_EQ (gv::ocr::canonical_locale ("PT-br"), "pt-BR");
   EXPECT_EQ (gv::ocr::canonical_locale ("zh-hant"), "zh-Hant");
   EXPECT_FALSE (gv::ocr::canonical_locale ("it").has_value ());
}

TEST_F (GameLocaleTest, ReadsCultureFromGameSettings)
{
   std::ofstream { path }
      << "GameUserSettingControlsSaved=(mouseSensitivity=0.5,culture=\"fr\",volume=1.0)\n";
   EXPECT_EQ (gv::ocr::read_game_locale (path), "fr");
}

TEST_F (GameLocaleTest, RejectsMissingAndUnsupportedCulture)
{
   std::ofstream { path } << "culture=\"it\"\n";
   EXPECT_FALSE (gv::ocr::read_game_locale (path).has_value ());
   std::filesystem::remove (path);
   EXPECT_FALSE (gv::ocr::read_game_locale (path).has_value ());
}

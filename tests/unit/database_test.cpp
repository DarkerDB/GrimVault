#include <gv/db/database.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <random>
#include <string>

using namespace gv;

namespace {

   std::filesystem::path make_tmp_dir (const std::filesystem::path& name)
   {
      std::random_device              rd;
      std::uniform_int_distribution<> dist (100000, 999999);

      auto p = std::filesystem::temp_directory_path () /
               ("grimvault_test_" + std::to_string (dist (rd))) / name;

      std::filesystem::create_directories (p);
      return p;
   }

}

TEST (Database, OpensUnderNonAsciiPath)
{
   const auto dir = make_tmp_dir (std::filesystem::path { u8"mikołaj 中文" });

   auto db = db::Database::open (dir / "grimvault.db");

   ASSERT_TRUE (db.has_value ()) << db.error ().message;
   EXPECT_TRUE (std::filesystem::exists (dir / "grimvault.db"));

   db.value ().reset ();
   std::filesystem::remove_all (dir.parent_path ());
}

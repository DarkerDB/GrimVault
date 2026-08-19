#include <gv/core/ini_migrator.h>
#include <gv/core/logger.h>
#include <gv/db/database.h>
#include <gv/db/repos/user_hotkeys_repo.h>
#include <gv/db/repos/user_settings_repo.h>

#include <SQLiteCpp/SQLiteCpp.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>

using namespace gv;

namespace {

   std::filesystem::path make_tmp_dir ()
   {
      std::random_device              rd;
      std::uniform_int_distribution<> dist (100000, 999999);

      auto p = std::filesystem::temp_directory_path () /
               ("grimvault_test_" + std::to_string (dist (rd)));

      std::filesystem::create_directories (p);
      return p;
   }

   void write_fixture_ini (const std::filesystem::path& p)
   {
      std::ofstream out { p };
      out << "[general]\n"
             "telemetry        = false\n"
             "scale            = 1.5\n"
             "default_mode     = manual\n"
             "alignment        = top-right\n"
             "components       = header,pricing\n"
             "\n"
             "[hotkeys]\n"
             "toggle_mode     = Ctrl+F6\n"
             "scan_now        = Ctrl+F5\n"
             "run_price_check = Ctrl+F5\n";
   }

   struct Fixture {
      std::filesystem::path        dir;
      std::unique_ptr<db::Database> database;

      Fixture ()
      {
         dir = make_tmp_dir ();
         core::Logger::init (dir / "logs");

         auto opened = db::Database::open (dir / "grimvault.db");
         EXPECT_TRUE (opened.has_value ());
         database = std::move (*opened);
      }

      ~Fixture ()
      {
         std::error_code ec;
         std::filesystem::remove_all (dir, ec);
      }
   };

} // namespace

TEST (IniMigratorTest, MigratesIniFromExistingInstall)
{
   Fixture                    f;
   db::UserSettingsRepo       settings { *f.database };
   db::UserHotkeysRepo        hotkeys  { *f.database };

   const auto ini = f.dir / "settings.ini";
   write_fixture_ini (ini);

   auto r = core::IniMigrator::run (ini, settings, hotkeys);
   ASSERT_TRUE (r.has_value ()) << r.error ().message;
   EXPECT_TRUE (*r);

   auto get_or_fail = [&] (auto key) {
      auto v = settings.get (key);
      EXPECT_TRUE (v.has_value ());
      EXPECT_TRUE (v->has_value ());
      return **v;
   };

   EXPECT_EQ (get_or_fail ("general:telemetry"),    "false");
   EXPECT_EQ (get_or_fail ("general:scale"),        "1.5");
   EXPECT_EQ (get_or_fail ("general:default_mode"), "manual");
   EXPECT_EQ (get_or_fail ("general:alignment"),    "top-right");
   EXPECT_EQ (get_or_fail ("general:components"),   "header,pricing");

   auto hk_toggle = hotkeys.get ("toggle_mode");
   ASSERT_TRUE (hk_toggle.has_value ());
   EXPECT_FALSE (hk_toggle->has_value ());

   auto hk_scan = hotkeys.get ("scan_now");
   ASSERT_TRUE (hk_scan.has_value ());
   ASSERT_TRUE (hk_scan->has_value ());
   EXPECT_EQ (**hk_scan, "Ctrl+F5");

   auto hk_run = hotkeys.get ("run_price_check");
   ASSERT_TRUE (hk_run.has_value ());
   ASSERT_TRUE (hk_run->has_value ());
   EXPECT_EQ (**hk_run, "Ctrl+F5");

   EXPECT_FALSE (std::filesystem::exists (ini));
   EXPECT_TRUE  (std::filesystem::exists (ini.string () + ".migrated"));
}

TEST (IniMigratorTest, IsIdempotent)
{
   Fixture              f;
   db::UserSettingsRepo settings { *f.database };
   db::UserHotkeysRepo  hotkeys  { *f.database };

   const auto ini = f.dir / "settings.ini";
   write_fixture_ini (ini);

   auto first = core::IniMigrator::run (ini, settings, hotkeys);
   ASSERT_TRUE (first.has_value ());
   ASSERT_TRUE (*first);

   auto second = core::IniMigrator::run (ini, settings, hotkeys);
   ASSERT_TRUE (second.has_value ());
   EXPECT_FALSE (*second);
}

TEST (IniMigratorTest, WritesDefaultsWhenNoIniPresent)
{
   Fixture              f;
   db::UserSettingsRepo settings { *f.database };
   db::UserHotkeysRepo  hotkeys  { *f.database };

   const auto ini = f.dir / "settings.ini";
   ASSERT_FALSE (std::filesystem::exists (ini));

   auto r = core::IniMigrator::run (ini, settings, hotkeys);
   ASSERT_TRUE (r.has_value ()) << r.error ().message;
   EXPECT_TRUE (*r);

   auto v = settings.get ("general:telemetry");
   ASSERT_TRUE (v.has_value ());
   ASSERT_TRUE (v->has_value ());
   EXPECT_EQ (**v, "true");

   auto hk_scan = hotkeys.get ("scan_now");
   ASSERT_TRUE (hk_scan.has_value ());
   ASSERT_TRUE (hk_scan->has_value ());
   EXPECT_EQ (**hk_scan, "F5");

   for (const auto action : { "toggle_mode", "debug_toggle", "clear_overlay" }) {
      auto hk = hotkeys.get (action);
      ASSERT_TRUE (hk.has_value ());
      EXPECT_FALSE (hk->has_value ());
   }
}

TEST (DatabaseMigrationTest, RetiresLocalHotkeys)
{
   const auto dir  = make_tmp_dir ();
   const auto path = dir / "grimvault.db";
   core::Logger::init (dir / "logs");

   {
      SQLite::Database database {
         path.string (), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE
      };
      database.exec (R"sql(
         CREATE TABLE user_hotkeys (action_id TEXT PRIMARY KEY, accelerator TEXT NOT NULL);
         CREATE TABLE pricing_cache (cache_key TEXT);
         CREATE TABLE item_finds (find_id INTEGER, found_at INTEGER, session_id INTEGER);
         CREATE TABLE session_runs (session_id INTEGER, ended_at INTEGER);
         INSERT INTO user_hotkeys VALUES ('scan_now', 'F5');
         INSERT INTO user_hotkeys VALUES ('toggle_mode', 'F6');
         INSERT INTO user_hotkeys VALUES ('debug_toggle', 'F7');
         INSERT INTO user_hotkeys VALUES ('clear_overlay', 'F8');
         PRAGMA user_version = 2;
      )sql");
   }

   auto database = db::Database::open (path);
   ASSERT_TRUE (database.has_value ());
   db::UserHotkeysRepo hotkeys { **database };

   auto scan = hotkeys.get ("scan_now");
   ASSERT_TRUE (scan.has_value ());
   ASSERT_TRUE (scan->has_value ());
   EXPECT_EQ (**scan, "F5");

   for (const auto action : { "toggle_mode", "debug_toggle", "clear_overlay" }) {
      auto hotkey = hotkeys.get (action);
      ASSERT_TRUE (hotkey.has_value ());
      EXPECT_FALSE (hotkey->has_value ());
   }

   database->reset ();
   std::error_code ec;
   std::filesystem::remove_all (dir, ec);
}

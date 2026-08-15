#include <gv/core/diagnostics.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace diag = gv::core::diagnostics;

std::filesystem::path scratch_dir (const char* name)
{
   auto dir = std::filesystem::temp_directory_path () / "grimvault-diagnostics" / name;
   std::filesystem::remove_all (dir);
   std::filesystem::create_directories (dir);
   return dir;
}

bool hex_token (const std::string& value)
{
   if (value.size () != 16) return false;
   return value.find_first_not_of ("0123456789abcdef") == std::string::npos;
}

TEST (Diagnostics, SessionIdIsStableHexWithinProcess)
{
   const auto& first = diag::session_id ();

   EXPECT_TRUE (hex_token (first)) << first;
   EXPECT_EQ (first, diag::session_id ());
}

TEST (Diagnostics, InstallIdPersistsToDisk)
{
   const auto dir = scratch_dir ("persists");

   const auto value = diag::install_id (dir);
   ASSERT_TRUE (hex_token (value)) << value;

   std::ifstream in { dir / "install-id" };
   ASSERT_TRUE (in.good ());

   std::string stored;
   std::getline (in, stored);
   EXPECT_EQ (stored, value);
}

TEST (Diagnostics, InstallIdReusesExistingToken)
{
   const auto dir = scratch_dir ("reuse");
   const std::string seeded = "0123456789abcdef";

   { std::ofstream out { dir / "install-id" }; out << seeded << '\n'; }

   EXPECT_EQ (diag::install_id (dir), seeded);
}

TEST (Diagnostics, MachineReportsAtLeastOneLine)
{
   const auto lines = diag::machine ();

   ASSERT_FALSE (lines.empty ());
   EXPECT_NE (lines.front ().find ("cpu="), std::string::npos) << lines.front ();
}

TEST (Diagnostics, ProcessSampleReportsAfterASecondReading)
{
   diag::process_sample ();

#ifdef _WIN32
   const auto sample = diag::process_sample ();
   EXPECT_NE (sample.find ("cpu="), std::string::npos) << sample;
   EXPECT_NE (sample.find ("rss="), std::string::npos) << sample;
#else
   EXPECT_TRUE (diag::process_sample ().empty ());
#endif
}

} // namespace

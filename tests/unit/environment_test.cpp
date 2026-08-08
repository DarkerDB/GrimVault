#include <gv/core/environment.h>

#include <gtest/gtest.h>

#include <cstdlib>

namespace {

int set_variable (const char* name, const char* value)
{
#ifdef _WIN32
   return _putenv_s (name, value);
#else
   return value [0] == '\0' ? unsetenv (name) : setenv (name, value, 1);
#endif
}

} // namespace

TEST (EnvironmentTest, ReadsAndClearsVariablesPortably)
{
   constexpr auto name = "GRIMVAULT_TEST_ENVIRONMENT_READ";

   ASSERT_EQ (set_variable (name, "active"), 0);
   EXPECT_EQ (gv::core::environment::get (name), "active");

   ASSERT_EQ (set_variable (name, ""), 0);
   EXPECT_TRUE (gv::core::environment::get (name).empty ());
}

#include <gv/core/environment.h>
#include <gv/core/env_resolver.h>

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

int set_variable (const char* name, const char* value)
{
#ifdef _WIN32
   return _putenv_s (name, value);
#else
   return value [0] == '\0' ? unsetenv (name) : setenv (name, value, 1);
#endif
}

class ScopedVariable
{
public:
   explicit ScopedVariable (const char* name)
      : name_ { name }, original_ { gv::core::environment::get (name) }
   {}

   ~ScopedVariable () { set_variable (name_.c_str (), original_.c_str ()); }

private:
   std::string name_;
   std::string original_;
};

} // namespace

TEST (EnvironmentTest, ReadsAndClearsVariablesPortably)
{
   constexpr auto name = "GRIMVAULT_TEST_ENVIRONMENT_READ";

   ASSERT_EQ (set_variable (name, "active"), 0);
   EXPECT_EQ (gv::core::environment::get (name), "active");

   ASSERT_EQ (set_variable (name, ""), 0);
   EXPECT_TRUE (gv::core::environment::get (name).empty ());
}

TEST (EnvironmentTest, AppEnvNeverOverridesTheBuildEnvironment)
{
   ScopedVariable app_env { "APP_ENV" };
   ScopedVariable grimvault_env { "GRIMVAULT_ENV" };

   ASSERT_EQ (set_variable ("APP_ENV", "qa"), 0);
   ASSERT_EQ (set_variable ("GRIMVAULT_ENV", ""), 0);

   EXPECT_EQ (gv::core::resolve_active_env ().name, gv::core::k_env_default.name);
}

TEST (EnvironmentTest, RuntimeEnvironmentOverrideFollowsTheBuildPolicy)
{
   ScopedVariable grimvault_env { "GRIMVAULT_ENV" };
   ASSERT_EQ (set_variable ("GRIMVAULT_ENV", "qa"), 0);

   const auto& selected = gv::core::resolve_active_env ();
   EXPECT_EQ (selected.name, gv::core::k_runtime_env_overrides_enabled
      ? "qa"
      : gv::core::k_env_default.name);
}

TEST (EnvironmentTest, CliEnvironmentOverrideIsConsumedAndFollowsBuildPolicy)
{
   ScopedVariable grimvault_env { "GRIMVAULT_ENV" };
   ASSERT_EQ (set_variable ("GRIMVAULT_ENV", ""), 0);

   std::vector<std::string> arguments { "--env", "qa", "status" };
   const auto& selected = gv::core::resolve_active_env (arguments);

   EXPECT_EQ (arguments, std::vector<std::string> { "status" });
   EXPECT_EQ (selected.name, gv::core::k_runtime_env_overrides_enabled
      ? "qa"
      : gv::core::k_env_default.name);
}

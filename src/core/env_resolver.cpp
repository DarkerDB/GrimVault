#include <gv/core/env_resolver.h>

#include <cstdlib>

namespace gv::core {

const EnvDef& env_for_name (std::string_view name)
{
   if (name == k_env_dev.name)  return k_env_dev;
   if (name == k_env_qa.name)   return k_env_qa;
   if (name == k_env_prod.name) return k_env_prod;
   return k_env_default;
}

namespace {

   std::string getenv_or_empty (const char* name)
   {
      if (const char* v = std::getenv (name); v && *v) return std::string { v };
      return {};
   }

} // namespace

const EnvDef& resolve_active_env (std::vector<std::string>& argv)
{
   // 1. --env <name> in argv.
   for (std::size_t i = 0; i + 1 < argv.size (); ++i) {
      if (argv [i] == "--env") {
         const auto& name = argv [i + 1];
         const auto& def  = env_for_name (name);
         argv.erase (argv.begin () + static_cast<std::ptrdiff_t> (i),
                     argv.begin () + static_cast<std::ptrdiff_t> (i) + 2);
         return def;
      }
      // --env=<name> form
      if (argv [i].rfind ("--env=", 0) == 0) {
         const auto name = argv [i].substr (6);
         const auto& def = env_for_name (name);
         argv.erase (argv.begin () + static_cast<std::ptrdiff_t> (i));
         return def;
      }
   }
   // Also handle --env=<name> in the last position (loop above skips the last).
   if (!argv.empty () && argv.back ().rfind ("--env=", 0) == 0) {
      const auto name = argv.back ().substr (6);
      const auto& def = env_for_name (name);
      argv.pop_back ();
      return def;
   }

   // 2. Environment variable.
   if (const auto v = getenv_or_empty ("GRIMVAULT_ENV"); !v.empty ()) {
      return env_for_name (v);
   }

   // 3. Compile-time default.
   return k_env_default;
}

const EnvDef& resolve_active_env ()
{
   if (const auto v = getenv_or_empty ("GRIMVAULT_ENV"); !v.empty ()) {
      return env_for_name (v);
   }
   return k_env_default;
}

namespace {

   const EnvDef* g_active = nullptr;

} // namespace

void set_active_env (const EnvDef& e)
{
   g_active = &e;
}

const EnvDef& active_env ()
{
   return g_active ? *g_active : k_env_default;
}

} // namespace gv::core

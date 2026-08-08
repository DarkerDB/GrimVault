#include <gv/core/env_resolver.h>
#include <gv/core/environment.h>

namespace gv::core {

namespace {

std::string consume_cli_env_override (std::vector<std::string>& argv)
{
   for (std::size_t i = 0; i < argv.size (); ++i) {
      if (argv [i].rfind ("--env=", 0) == 0) {
         const auto name = argv [i].substr (6);
         argv.erase (argv.begin () + static_cast<std::ptrdiff_t> (i));
         return name;
      }

      if (argv [i] == "--env" && i + 1 < argv.size ()) {
         const auto name = argv [i + 1];
         argv.erase (argv.begin () + static_cast<std::ptrdiff_t> (i),
                     argv.begin () + static_cast<std::ptrdiff_t> (i) + 2);
         return name;
      }
   }
   return {};
}

} // namespace

const EnvDef& env_for_name (std::string_view name)
{
   if (name == k_env_dev.name)  return k_env_dev;
   if (name == k_env_qa.name)   return k_env_qa;
   if (name == k_env_prod.name) return k_env_prod;
   return k_env_default;
}

const EnvDef& resolve_active_env (std::vector<std::string>& argv)
{
   const auto requested = consume_cli_env_override (argv);
   if (!k_runtime_env_overrides_enabled) return k_env_default;
   if (!requested.empty ()) return env_for_name (requested);

   if (const auto v = environment::get ("GRIMVAULT_ENV"); !v.empty ()) {
      return env_for_name (v);
   }

   return k_env_default;
}

const EnvDef& resolve_active_env ()
{
   if (!k_runtime_env_overrides_enabled) return k_env_default;
   if (const auto v = environment::get ("GRIMVAULT_ENV"); !v.empty ()) {
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

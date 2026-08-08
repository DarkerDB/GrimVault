#include <gv/core/environment.h>

#include <cstdlib>

namespace gv::core::environment {

std::string get (const char* name)
{
#ifdef _WIN32
   char* value = nullptr;
   std::size_t size = 0;
   if (_dupenv_s (&value, &size, name) != 0 || !value) return {};

   std::string result { value };
   std::free (value);
   return result;
#else
   if (const char* value = std::getenv (name); value && *value) {
      return std::string { value };
   }
   return {};
#endif
}

} // namespace gv::core::environment

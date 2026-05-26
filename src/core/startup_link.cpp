#include <gv/core/startup_link.h>
#include <gv/core/logger.h>

#include <Windows.h>

#include <string>

namespace gv::core {

namespace {

   constexpr const wchar_t* k_run_subkey =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

   std::wstring widen (std::string_view s)
   {
      if (s.empty ()) return {};
      const int n = ::MultiByteToWideChar (CP_UTF8, 0, s.data (),
         static_cast<int> (s.size ()), nullptr, 0);
      std::wstring out (static_cast<std::size_t> (n), L'\0');
      ::MultiByteToWideChar (CP_UTF8, 0, s.data (),
         static_cast<int> (s.size ()), out.data (), n);
      return out;
   }

   std::string narrow (const std::wstring& s)
   {
      if (s.empty ()) return {};
      const int n = ::WideCharToMultiByte (CP_UTF8, 0, s.c_str (),
         static_cast<int> (s.size ()), nullptr, 0, nullptr, nullptr);
      std::string out (static_cast<std::size_t> (n), '\0');
      ::WideCharToMultiByte (CP_UTF8, 0, s.c_str (),
         static_cast<int> (s.size ()), out.data (), n, nullptr, nullptr);
      return out;
   }

} // namespace

Result<void> StartupLink::enable (
   std::string_view app_name,
   std::string_view exe_path,
   std::string_view args)
{
   HKEY key;
   if (::RegCreateKeyExW (HKEY_CURRENT_USER, k_run_subkey, 0, nullptr,
         REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
      return fail (Error::make (ErrorKind::Permission,
         "startup_link: cannot open HKCU Run key"));
   }

   std::wstring command = L"\"" + widen (exe_path) + L"\"";
   if (!args.empty ()) {
      command += L" ";
      command += widen (args);
   }

   const auto wname = widen (app_name);
   const auto rc = ::RegSetValueExW (key, wname.c_str (), 0, REG_SZ,
      reinterpret_cast<const BYTE*> (command.c_str ()),
      static_cast<DWORD> ((command.size () + 1) * sizeof (wchar_t)));

   ::RegCloseKey (key);

   if (rc != ERROR_SUCCESS) {
      return fail (Error::make (ErrorKind::Permission,
         "startup_link: RegSetValueExW failed (err={})", rc));
   }

   Logger::info ("startup_link: enabled '{}' → {}", app_name, narrow (command));
   return {};
}

Result<void> StartupLink::disable (std::string_view app_name)
{
   HKEY key;
   if (::RegOpenKeyExW (HKEY_CURRENT_USER, k_run_subkey, 0,
         KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
      return {};  // key doesn't exist, nothing to remove
   }

   const auto wname = widen (app_name);
   ::RegDeleteValueW (key, wname.c_str ());
   ::RegCloseKey (key);

   Logger::info ("startup_link: disabled '{}'", app_name);
   return {};
}

Result<bool> StartupLink::is_enabled (std::string_view app_name)
{
   HKEY key;
   if (::RegOpenKeyExW (HKEY_CURRENT_USER, k_run_subkey, 0,
         KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
      return false;
   }

   const auto wname = widen (app_name);
   const auto rc = ::RegQueryValueExW (key, wname.c_str (),
      nullptr, nullptr, nullptr, nullptr);

   ::RegCloseKey (key);

   return rc == ERROR_SUCCESS;
}

} // namespace gv::core

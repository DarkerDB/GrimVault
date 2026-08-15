#include <gv/core/diagnostics.h>

#include <gv/core/logger.h>

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
   #define WIN32_LEAN_AND_MEAN
   #include <windows.h>
   #include <psapi.h>
   #include <shellscalingapi.h>
   #pragma comment (lib, "Shcore.lib")
#endif

namespace gv::core::diagnostics {

namespace {

   std::string random_token ()
   {
      std::random_device device;
      std::mt19937_64 engine { (static_cast<std::uint64_t> (device ()) << 32) ^ device () };
      return fmt::format ("{:016x}", engine ());
   }

   bool plausible_token (std::string_view value)
   {
      if (value.size () != 16) return false;
      for (const char c : value) {
         const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         if (!hex) return false;
      }
      return true;
   }

#ifdef _WIN32

   std::string registry_cpu_name ()
   {
      HKEY key {};
      const auto path = R"(HARDWARE\DESCRIPTION\System\CentralProcessor\0)";
      if (::RegOpenKeyExA (HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
         return "unknown";
      }

      char buffer [256] {};
      DWORD size = sizeof (buffer);
      DWORD type = 0;
      const auto rc = ::RegQueryValueExA (
         key, "ProcessorNameString", nullptr, &type,
         reinterpret_cast<LPBYTE> (buffer), &size);
      ::RegCloseKey (key);

      if (rc != ERROR_SUCCESS || type != REG_SZ) return "unknown";

      std::string name { buffer };
      while (!name.empty () && (name.back () == ' ' || name.back () == '\0')) name.pop_back ();
      return name.empty () ? "unknown" : name;
   }

   std::string os_build ()
   {
      using RtlGetVersionFn = LONG (WINAPI*) (PRTL_OSVERSIONINFOW);

      if (auto* mod = ::GetModuleHandleW (L"ntdll.dll")) {
         if (auto fn = reinterpret_cast<RtlGetVersionFn> (
                ::GetProcAddress (mod, "RtlGetVersion"))) {
            RTL_OSVERSIONINFOW info {};
            info.dwOSVersionInfoSize = sizeof (info);
            if (fn (&info) == 0) {
               return fmt::format ("{}.{}.{}",
                  info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
            }
         }
      }
      return "unknown";
   }

   std::vector<std::string> adapters ()
   {
      std::vector<std::string> out;
      DISPLAY_DEVICEA device {};
      device.cb = sizeof (device);

      for (DWORD i = 0; ::EnumDisplayDevicesA (nullptr, i, &device, 0); ++i) {
         if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;
         std::string name { device.DeviceString };
         if (std::find (out.begin (), out.end (), name) == out.end ()) {
            out.push_back (std::move (name));
         }
      }
      return out;
   }

   BOOL CALLBACK collect_monitor (HMONITOR monitor, HDC, LPRECT, LPARAM param)
   {
      auto* out = reinterpret_cast<std::vector<std::string>*> (param);

      MONITORINFOEXA info {};
      info.cbSize = sizeof (info);
      if (!::GetMonitorInfoA (monitor, &info)) return TRUE;

      UINT dpi_x = 96;
      UINT dpi_y = 96;
      ::GetDpiForMonitor (monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y);

      DEVMODEA mode {};
      mode.dmSize = sizeof (mode);
      const bool have_mode = ::EnumDisplaySettingsA (
         info.szDevice, ENUM_CURRENT_SETTINGS, &mode) != 0;

      out->push_back (fmt::format (
         "display {} {}x{} {}Hz scale={}%{}",
         out->size (),
         have_mode ? static_cast<long> (mode.dmPelsWidth)
                   : info.rcMonitor.right - info.rcMonitor.left,
         have_mode ? static_cast<long> (mode.dmPelsHeight)
                   : info.rcMonitor.bottom - info.rcMonitor.top,
         have_mode ? mode.dmDisplayFrequency : 0,
         (dpi_x * 100) / 96,
         (info.dwFlags & MONITORINFOF_PRIMARY) ? " primary" : ""));

      return TRUE;
   }

#endif

   std::mutex          g_sample_mutex;
   std::uint64_t       g_last_kernel  = 0;
   std::uint64_t       g_last_user    = 0;
   std::uint64_t       g_last_wall_us = 0;

} // namespace

const std::string& session_id ()
{
   static const std::string value = random_token ();
   return value;
}

const std::string& install_id (const std::filesystem::path& data_dir)
{
   static std::mutex mutex;
   static std::map<std::filesystem::path, std::string> cache;

   std::lock_guard lock { mutex };

   if (const auto it = cache.find (data_dir); it != cache.end ()) return it->second;

   const auto path = data_dir / "install-id";

   if (std::ifstream in { path }) {
      std::string stored;
      std::getline (in, stored);
      while (!stored.empty () && (stored.back () == '\r' || stored.back () == '\n')) {
         stored.pop_back ();
      }
      if (plausible_token (stored)) {
         return cache.emplace (data_dir, std::move (stored)).first->second;
      }
   }

   auto value = random_token ();

   std::error_code ec;
   std::filesystem::create_directories (data_dir, ec);
   if (std::ofstream out { path, std::ios::trunc }) {
      out << value << '\n';
   } else {
      Logger::warn ("diagnostics: install id not persisted ({})", path.string ());
   }

   return cache.emplace (data_dir, std::move (value)).first->second;
}

std::vector<std::string> machine ()
{
   std::vector<std::string> lines;

#ifdef _WIN32
   MEMORYSTATUSEX memory {};
   memory.dwLength = sizeof (memory);
   ::GlobalMemoryStatusEx (&memory);

   SYSTEM_INFO system {};
   ::GetSystemInfo (&system);

   lines.push_back (fmt::format ("cpu=\"{}\" logical={} ram={:.1f}GB os={}",
      registry_cpu_name (),
      system.dwNumberOfProcessors,
      static_cast<double> (memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0),
      os_build ()));

   for (const auto& adapter : adapters ()) {
      lines.push_back (fmt::format ("gpu=\"{}\"", adapter));
   }

   std::vector<std::string> monitors;
   ::EnumDisplayMonitors (nullptr, nullptr, &collect_monitor,
      reinterpret_cast<LPARAM> (&monitors));
   for (auto& monitor : monitors) lines.push_back (std::move (monitor));
#else
   lines.push_back (fmt::format ("cpu=unknown logical={} ram=unknown os=unknown",
      std::thread::hardware_concurrency ()));
#endif

   return lines;
}

std::string process_sample ()
{
#ifdef _WIN32
   FILETIME creation {};
   FILETIME exit {};
   FILETIME kernel {};
   FILETIME user {};
   if (!::GetProcessTimes (::GetCurrentProcess (), &creation, &exit, &kernel, &user)) {
      return {};
   }

   const auto to_us = [] (const FILETIME& ft) {
      return ((static_cast<std::uint64_t> (ft.dwHighDateTime) << 32)
         | ft.dwLowDateTime) / 10;
   };

   const auto wall_us = static_cast<std::uint64_t> (
      std::chrono::duration_cast<std::chrono::microseconds> (
         std::chrono::steady_clock::now ().time_since_epoch ()).count ());

   PROCESS_MEMORY_COUNTERS memory {};
   ::GetProcessMemoryInfo (::GetCurrentProcess (), &memory, sizeof (memory));

   SYSTEM_INFO system {};
   ::GetSystemInfo (&system);

   std::lock_guard lock { g_sample_mutex };

   const auto kernel_us  = to_us (kernel);
   const auto user_us    = to_us (user);
   const auto prev_busy  = g_last_kernel + g_last_user;
   const auto prev_wall  = g_last_wall_us;

   g_last_kernel  = kernel_us;
   g_last_user    = user_us;
   g_last_wall_us = wall_us;

   if (prev_wall == 0 || wall_us <= prev_wall) return {};

   const auto busy_us = (kernel_us + user_us) - prev_busy;
   const auto elapsed = wall_us - prev_wall;
   const auto cores   = std::max<DWORD> (1, system.dwNumberOfProcessors);

   return fmt::format ("cpu={:.1f}% cpu_core={:.1f}% rss={}MB",
      (100.0 * static_cast<double> (busy_us)) / (static_cast<double> (elapsed) * cores),
      (100.0 * static_cast<double> (busy_us)) / static_cast<double> (elapsed),
      memory.WorkingSetSize / (1024 * 1024));
#else
   return {};
#endif
}

} // namespace gv::core::diagnostics

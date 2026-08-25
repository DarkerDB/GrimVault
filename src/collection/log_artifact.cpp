#include <gv/collection/log_artifact.h>

#include <gv/collection/collector.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

namespace gv::collection {

namespace {

constexpr std::uintmax_t max_log_bytes = 16 * 1024 * 1024;

std::string local_date ()
{
   const auto now = std::chrono::system_clock::to_time_t (std::chrono::system_clock::now ());
   std::tm parts {};
#ifdef _WIN32
   ::localtime_s (&parts, &now);
#else
   ::localtime_r (&now, &parts);
#endif
   char value [11] {};
   std::strftime (value, sizeof (value), "%Y-%m-%d", &parts);
   return value;
}

std::string date_of (const std::filesystem::path& path)
{
   const auto name = path.filename ().string ();
   constexpr std::string_view prefix = "grimvault_";
   constexpr std::string_view suffix = ".txt";
   if (!name.starts_with (prefix) || !name.ends_with (suffix)
       || name.size () != prefix.size () + 10 + suffix.size ()) return {};

   const auto date = name.substr (prefix.size (), 10);
   if (date [4] != '-' || date [7] != '-') return {};
   for (std::size_t i = 0; i < date.size (); ++i) {
      if (i == 4 || i == 7) continue;
      if (date [i] < '0' || date [i] > '9') return {};
   }
   return date;
}

}

bool submit_latest_log (
   Collector& collector,
   const std::filesystem::path& directory,
   std::string_view install_id,
   std::string_view version,
   std::string_view environment)
{
   if (!collector.enabled ()) return false;

   const auto today = local_date ();
   std::filesystem::path latest;
   std::string latest_date;
   std::error_code error;

   for (std::filesystem::directory_iterator files { directory, error }, end;
        !error && files != end; files.increment (error)) {
      if (!files->is_regular_file (error) || error) break;
      const auto date = date_of (files->path ());
      if (date.empty () || date >= today || date <= latest_date) continue;
      const auto bytes = files->file_size (error);
      if (error || bytes == 0 || bytes > max_log_bytes) {
         error.clear ();
         continue;
      }
      latest = files->path ();
      latest_date = date;
   }

   if (latest.empty ()) return false;
   std::ifstream input { latest, std::ios::binary };
   if (!input) return false;
   std::string body (
      std::istreambuf_iterator<char> { input },
      std::istreambuf_iterator<char> {}
   );
   if (body.empty ()) return false;

   return collector.submit ({
      .channel = "log",
      .content_type = "text/plain",
      .body = std::move (body),
      .metadata = {
         { "schema", 1 },
         { "partition", "logs" },
         { "install_id", install_id },
         { "date", latest_date },
         { "filename", latest.filename ().string () },
         { "version", version },
         { "environment", environment },
      },
   });
}

}

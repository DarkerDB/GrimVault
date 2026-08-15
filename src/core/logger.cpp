#include <gv/core/logger.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <chrono>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gv::core {

namespace {

   std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_ring_sink;
   std::once_flag g_init_flag;

   std::mutex               g_header_mutex;
   std::vector<std::string> g_header;
   int                      g_header_day = -1;

   int local_day ()
   {
      const auto now = std::chrono::system_clock::to_time_t (
         std::chrono::system_clock::now ());
      std::tm parts {};
#ifdef _WIN32
      ::localtime_s (&parts, &now);
#else
      ::localtime_r (&now, &parts);
#endif
      return parts.tm_year * 512 + parts.tm_yday;
   }

   void write_header ()
   {
      for (const auto& line : g_header) spdlog::info ("[session] {}", line);
   }

   void ensure_header ()
   {
      std::lock_guard lock { g_header_mutex };
      if (g_header.empty ()) return;

      const int day = local_day ();
      if (day == g_header_day) return;

      g_header_day = day;
      write_header ();
   }

   void do_init (const std::filesystem::path& log_dir, bool verbose)
   {
      std::error_code ec;
      std::filesystem::create_directories (log_dir, ec);

      // dev-run redirects the GUI-subsystem process through a pipe so it can
      // wait for it and display its output. Keep this sink in automatic mode:
      // forcing the Windows console-color implementation against a redirected
      // handle can make writes disappear. The launcher adds colors after it
      // receives each plain-text line; file and ring sinks stay plain too.
      auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt> ();
      stdout_sink->set_pattern ("[%H:%M:%S.%e] [%l] %v");

      auto file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt> (
         (log_dir / "grimvault.txt").string (),
         0, 0,
         false,
         7
      );
      file_sink->set_pattern ("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");

      g_ring_sink = std::make_shared<spdlog::sinks::ringbuffer_sink_mt> (1024);
      g_ring_sink->set_pattern ("[%H:%M:%S.%e] [%l] %v");

      std::vector<spdlog::sink_ptr> sinks {
         stdout_sink,
         file_sink,
         g_ring_sink,
      };

      auto logger = std::make_shared<spdlog::logger> (
         "grimvault",
         sinks.begin (),
         sinks.end ()
      );

      logger->set_level (verbose ? spdlog::level::debug : spdlog::level::info);
      logger->flush_on (spdlog::level::warn);

      spdlog::set_default_logger (logger);

      // Background flush so info-level lines reach the file promptly. With
      // only flush_on(warn), a healthy session (no warnings) can sit on
      // minutes of buffered startup lines — tailing the log then looks like
      // a hang at whatever the last warning was.
      spdlog::flush_every (std::chrono::seconds (2));
   }

} // namespace

void Logger::init (const std::filesystem::path& log_dir, bool verbose)
{
   std::call_once (g_init_flag, [&] { do_init (log_dir, verbose); });
}

void Logger::shutdown ()
{
   spdlog::shutdown ();
   g_ring_sink.reset ();
}

void Logger::info  (std::string_view msg) { ensure_header (); spdlog::info  ("{}", msg); }
void Logger::warn  (std::string_view msg) { ensure_header (); spdlog::warn  ("{}", msg); }
void Logger::error (std::string_view msg) { ensure_header (); spdlog::error ("{}", msg); }
void Logger::debug (std::string_view msg) { ensure_header (); spdlog::debug ("{}", msg); }

void Logger::set_header (std::vector<std::string> lines)
{
   std::lock_guard lock { g_header_mutex };
   g_header     = std::move (lines);
   g_header_day = local_day ();
   write_header ();
}

std::vector<std::string> Logger::tail (std::size_t n)
{
   if (!g_ring_sink) {
      return {};
   }

   return g_ring_sink->last_formatted (n);
}

void Log::info  (std::string_view msg) const { ensure_header (); spdlog::info  ("[{}] {}", tag_, msg); }
void Log::warn  (std::string_view msg) const { ensure_header (); spdlog::warn  ("[{}] {}", tag_, msg); }
void Log::error (std::string_view msg) const { ensure_header (); spdlog::error ("[{}] {}", tag_, msg); }
void Log::debug (std::string_view msg) const { ensure_header (); spdlog::debug ("[{}] {}", tag_, msg); }

void Log::event (std::string_view name, Fields fields) const
{
   ensure_header ();

   std::string line;
   line.reserve (name.size () + 64);
   line.append (name.data (), name.size ());
   for (const auto& [k, v] : fields) {
      line.push_back (' ');
      line.append (k.data (), k.size ());
      line.push_back ('=');
      line.append (v);
   }
   spdlog::info ("[{}] {}", tag_, line);
}

} // namespace gv::core

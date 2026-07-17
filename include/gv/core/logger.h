#pragma once

#include <filesystem>
#include <fmt/core.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spdlog { class logger; }

namespace gv::core {

// Facade over spdlog. Routes to:
//    - stdout (color, %^%l%$ level)
//    - daily-rotated file at <log_dir>/grimvault_YYYY-MM-DD.txt (7-day keep)
//    - in-memory ring buffer (last 1024 lines) for the Diagnostics page
//
// Initialize once at app start via Logger::init(). Modules either call the
// static Logger helpers (anonymous lines) or hold a tagged `core::log::<mod>`
// instance — the tagged form prefixes lines with `[<mod>]`.
class Logger
{
public:
   static void init (const std::filesystem::path& log_dir, bool verbose = false);
   static void shutdown ();

   static void info  (std::string_view msg);
   static void warn  (std::string_view msg);
   static void error (std::string_view msg);
   static void debug (std::string_view msg);

   template <typename... Args>
   static void info (fmt::format_string<Args...> fmt_str, Args&&... args)
   {
      info (std::string_view { fmt::format (fmt_str, std::forward<Args> (args)...) });
   }

   template <typename... Args>
   static void warn (fmt::format_string<Args...> fmt_str, Args&&... args)
   {
      warn (std::string_view { fmt::format (fmt_str, std::forward<Args> (args)...) });
   }

   template <typename... Args>
   static void error (fmt::format_string<Args...> fmt_str, Args&&... args)
   {
      error (std::string_view { fmt::format (fmt_str, std::forward<Args> (args)...) });
   }

   template <typename... Args>
   static void debug (fmt::format_string<Args...> fmt_str, Args&&... args)
   {
      debug (std::string_view { fmt::format (fmt_str, std::forward<Args> (args)...) });
   }

   static std::vector<std::string> tail (std::size_t n = 100);
};

class Log
{
public:
   using Field  = std::pair<std::string_view, std::string>;
   using Fields = std::initializer_list<Field>;

   explicit Log (std::string_view tag) : tag_ (tag) {}

   void info  (std::string_view msg) const;
   void warn  (std::string_view msg) const;
   void error (std::string_view msg) const;
   void debug (std::string_view msg) const;

   template <typename... Args>
   void info (fmt::format_string<Args...> fmt_str, Args&&... args) const
   {
      info (std::string_view { fmt::format (fmt_str, std::forward<Args> (args)...) });
   }

   template <typename... Args>
   void warn (fmt::format_string<Args...> fmt_str, Args&&... args) const
   {
      warn (std::string_view { fmt::format (fmt_str, std::forward<Args> (args)...) });
   }

   template <typename... Args>
   void error (fmt::format_string<Args...> fmt_str, Args&&... args) const
   {
      error (std::string_view { fmt::format (fmt_str, std::forward<Args> (args)...) });
   }

   template <typename... Args>
   void debug (fmt::format_string<Args...> fmt_str, Args&&... args) const
   {
      debug (std::string_view { fmt::format (fmt_str, std::forward<Args> (args)...) });
   }

   void event (std::string_view name, Fields fields) const;

private:
   std::string tag_;
};

namespace log {

   inline const Log api      { "api" };
   inline const Log app      { "app" };
   inline const Log capture  { "capture" };
   inline const Log db       { "db" };
   inline const Log ocr      { "ocr" };
   inline const Log ui       { "ui" };
   inline const Log update   { "update" };
   inline const Log vision   { "vision" };

}

} // namespace gv::core

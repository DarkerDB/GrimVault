#pragma once

#include <fmt/core.h>
#include <source_location>
#include <string>
#include <string_view>

namespace gv::core {

enum class ErrorKind {
   InvalidArgument,
   NotFound,
   Permission,
   Io,
   Database,
   ExternalApi,
   Capture,
   Ocr,
   Internal,
};

struct Error {
   ErrorKind            kind;
   std::string          message;
   std::source_location where;

   Error (
      ErrorKind             kind,
      std::string           message,
      std::source_location  where = std::source_location::current ()
   )
      : kind    (kind),
        message (std::move (message)),
        where   (where)
   {}

   template <typename... Args>
   static Error make (
      ErrorKind             kind,
      fmt::format_string<Args...> fmt_str,
      Args&&...             args
   ) {
      return Error { kind, fmt::format (fmt_str, std::forward<Args> (args)...) };
   }
};

constexpr std::string_view kind_name (ErrorKind k) noexcept
{
   switch (k) {
      case ErrorKind::InvalidArgument: return "invalid_argument";
      case ErrorKind::NotFound:        return "not_found";
      case ErrorKind::Permission:      return "permission";
      case ErrorKind::Io:              return "io";
      case ErrorKind::Database:        return "database";
      case ErrorKind::ExternalApi:     return "external_api";
      case ErrorKind::Capture:         return "capture";
      case ErrorKind::Ocr:             return "ocr";
      case ErrorKind::Internal:        return "internal";
   }

   return "unknown";
}

} // namespace gv::core

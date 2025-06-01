#include "logger.h"
#include <sstream>
#include <iomanip>

Napi::ThreadSafeFunction Logger::callback;

void Logger::log (HRESULT Hr, const std::string &Message)
{
   std::ostringstream Oss;
   
   Oss << Message << " (HRESULT: 0x" << std::hex << std::uppercase << Hr << ")";
   
   LPSTR Error = nullptr;
   
   FormatMessageA (
      FORMAT_MESSAGE_FROM_SYSTEM | 
      FORMAT_MESSAGE_ALLOCATE_BUFFER |
      FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,
      Hr,
      MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
      reinterpret_cast<LPSTR> (&Error),
      0,
      nullptr
   );
   
   if (Error != nullptr) {
      Oss << " - " << Error;
      LocalFree (Error);
   }
   
   std::string S = Oss.str ();
   
   if (!S.empty () && S.back () == '\n') {
      S.pop_back ();
   }
   
   log (
      Logger::Level::E_ERROR,
      S
   );
}

void Logger::log (Level Level, const std::string &Message)
{
   if (callback) {
      auto Bind = [ Level, Message ] (Napi::Env Env, Napi::Function Callee) {
         Callee.Call ({
            Napi::String::New (Env, Logger::levelToString (Level)),
            Napi::String::New (Env, Message)
         });
      };
      
      callback.NonBlockingCall (Bind);
   }
}

std::string Logger::levelToString (Level Level) {
   switch (Level) {
      case Level::E_DEBUG:
         return "debug";
      case Level::E_INFO:
         return "info";
      case Level::E_WARNING:
         return "warn";
      case Level::E_ERROR:
         return "error";
      default:
         return "unknown";
   }
}

void Logger::initialize (Napi::ThreadSafeFunction Callback) {
   Logger::callback = Callback;
}
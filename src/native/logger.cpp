#include "logger.h"
#include <sstream>
#include <iomanip>

Napi::ThreadSafeFunction Logger::callback;

void Logger::log (HRESULT hr, const std::string &message)
{
    std::ostringstream oss;

    oss << message << " (HRESULT: 0x" << std::hex << std::uppercase << hr << ")";

    LPSTR error = nullptr;

    FormatMessageA (
        FORMAT_MESSAGE_FROM_SYSTEM | 
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        hr,
        MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR> (&error),
        0,
        nullptr
    );

    if (error != nullptr) {
        oss << " - " << error;
        LocalFree (error);
    }

    std::string s = oss.str ();

    if (!s.empty () && s.back () == '\n') {
        s.pop_back ();
    }

    log (
        Logger::Level::E_ERROR,
        s
    );
}

void Logger::log (Level level, const std::string &message)
{
    if (callback) {
        auto bind = [ level, message ] (Napi::Env env, Napi::Function callee) {
            callee.Call ({
                Napi::String::New (env, Logger::levelToString (level)),
                Napi::String::New (env, message)
            });
        };

        callback.NonBlockingCall (bind);
    }
}

std::string Logger::levelToString (Level level) {
    switch (level) {
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

void Logger::initialize (Napi::ThreadSafeFunction callback) {
    Logger::callback = callback;
}
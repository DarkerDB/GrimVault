#ifndef LOGGER_H
#define LOGGER_H

#include <napi.h>
#include <string>
#include <windows.h>

class Logger 
{
    public:
        enum class Level { 
            E_DEBUG, 
            E_INFO, 
            E_WARNING, 
            E_ERROR 
        };

        static void log (Level level, const std::string &message);
        static void log (HRESULT hr, const std::string &message);

        static void initialize (Napi::ThreadSafeFunction callback);

    private:
        static Napi::ThreadSafeFunction callback;

        static std::string levelToString (Level level);
};

#endif
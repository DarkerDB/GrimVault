#pragma once

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
   
   static void log (Level Level, const std::string &Message);
   static void log (HRESULT Hr, const std::string &Message);
   
   static void initialize (Napi::ThreadSafeFunction Callback);
   
   private:
   
   static Napi::ThreadSafeFunction callback;
   
   static std::string levelToString (Level Level);
};
#include "logger.h"
#include "windows.h"
#include <shellscalingapi.h>

// To get DPI scaling
#pragma comment(lib, "shcore.lib")

ActiveWindowWorker::ActiveWindowWorker (const Napi::Env& env) : Napi::AsyncWorker (env),
   deferred (Napi::Promise::Deferred::New (env)) 
{
}

void ActiveWindowWorker::Execute ()
{
   HWND handle = GetForegroundWindow ();
   
   if (!handle) {
      return;
   }
   
   int length = GetWindowTextLengthW (handle);
   
   if (length < 0) {
      return;
   }

   title.resize (length + 1);
   GetWindowTextW (handle, &title [0], length + 1);
   title.resize (length);
}

void ActiveWindowWorker::OnOK ()
{
   Napi::Env env = Env ();

   if (title.empty ()) {
      deferred.Resolve (env.Null ());
      return;
   }

   Napi::String result = Napi::String::New (
      env, 
      std::u16string (title.begin (), title.end ())
   );
   
   deferred.Resolve (result);
}

void ActiveWindowWorker::OnError (const Napi::Error &e)
{
   deferred.Reject (e.Value ());
}

Napi::Promise ActiveWindowWorker::GetPromise () 
{ 
   return deferred.Promise (); 
}

// -- -- //

GameWindowWorker::GameWindowWorker (const Napi::Env& env) : Napi::AsyncWorker (env),
   deferred (Napi::Promise::Deferred::New (env)) 
{
}

void GameWindowWorker::Execute ()
{
   window  = {};
   monitor = {};

   handle = FindWindowW (nullptr, L"Dark and Darker  ");
   
   if (!handle) {
      Logger::log (
         Logger::Level::E_DEBUG,
         "Game window not found"
      );

      return;
   }

   if (!IsWindowVisible (handle)) {
      Logger::log (
         Logger::Level::E_DEBUG,
         "Game window found but not visible"
      );

      return;
   }
   
   if (!GetWindowRect (handle, &window)) {
      Logger::log (
         Logger::Level::E_DEBUG,
         "Game window found and visible but its rectangle is not available"
      );

      return;
   }
   
   HMONITOR monitor = MonitorFromWindow (handle, MONITOR_DEFAULTTONEAREST);

   MONITORINFO monitorInfo = {};
   monitorInfo.cbSize = sizeof (MONITORINFO);
   
   if (GetMonitorInfo (monitor, &monitorInfo)) {
      this->monitor = monitorInfo.rcWork;
   }
   
   UINT dpiX;
   UINT dpiY;

   if (GetDpiForMonitor (monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) == S_OK) {
      scale = static_cast <double> (dpiX) / 96.0;
   }
}

void GameWindowWorker::OnOK ()
{
   Napi::Env env = Env ();
   
   if (!handle) {
      deferred.Resolve (env.Null ());
      return;
   }
      
   Napi::Object bounds = Napi::Object::New (env);
   
   bounds.Set ("x",       Napi::Number::New (env, window.left));
   bounds.Set ("y",       Napi::Number::New (env, window.top));
   bounds.Set ("width",   Napi::Number::New (env, window.right - window.left));
   bounds.Set ("height",  Napi::Number::New (env, window.bottom - window.top));
   
   Napi::Object monitor = Napi::Object::New (env);
   
   monitor.Set ("x",      Napi::Number::New (env, this->monitor.left));
   monitor.Set ("y",      Napi::Number::New (env, this->monitor.top));
   monitor.Set ("width",  Napi::Number::New (env, this->monitor.right - this->monitor.left));
   monitor.Set ("height", Napi::Number::New (env, this->monitor.bottom - this->monitor.top));
   monitor.Set ("scale",  Napi::Number::New (env, scale));

   Napi::Object result = Napi::Object::New (env);

   result.Set ("bounds", bounds);
   result.Set ("monitor", monitor);
   
   deferred.Resolve (result);
}

void GameWindowWorker::OnError (const Napi::Error &e)
{
   deferred.Reject (e.Value ());
}

Napi::Promise GameWindowWorker::GetPromise ()
{ 
   return deferred.Promise (); 
}
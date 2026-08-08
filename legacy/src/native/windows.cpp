#include "logger.h"
#include "windows.h"
#include <shellscalingapi.h>
#include <combaseapi.h>
#include <memory>
#include <functional>

// To get DPI scaling
#pragma comment(lib, "shcore.lib")

ActiveWindowWorker::ActiveWindowWorker (const Napi::Env& env) : Napi::AsyncWorker (env),
   deferred (Napi::Promise::Deferred::New (env)) 
{
}

void ActiveWindowWorker::Execute ()
{
   // Initialize COM for this thread to safely access COM objects
   HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
   bool comInitialized = SUCCEEDED(comResult);

   // Ensure COM cleanup on exit
   auto comCleanup = std::unique_ptr<void, std::function<void(void*)>>(
      &comResult,
      [comInitialized](void*) {
         if (comInitialized) {
            CoUninitialize();
         }
      }
   );

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

// -- Window Event Hook Implementation -- //

WindowEventHook& WindowEventHook::GetInstance ()
{
   static WindowEventHook instance;
   return instance;
}

WindowEventHook::~WindowEventHook ()
{
   Shutdown ();
}

bool WindowEventHook::Initialize (Napi::Env env, Napi::Function callback)
{
   std::lock_guard<std::mutex> lock (mutex_);

   if (initialized_) {
      Logger::log (Logger::Level::E_WARNING, "WindowEventHook already initialized");
      return false;
   }

   // Create thread-safe function for callbacks
   tsfn_ = Napi::ThreadSafeFunction::New (
      env,
      callback,
      "WindowEventCallback",
      0,
      1,
      [](Napi::Env) {}
   );

   // Hook for window location changes (move/resize)
   locationHook_ = SetWinEventHook (
      EVENT_OBJECT_LOCATIONCHANGE,
      EVENT_OBJECT_LOCATIONCHANGE,
      NULL,
      WinEventProc,
      0,
      0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
   );

   // Hook for foreground window changes
   foregroundHook_ = SetWinEventHook (
      EVENT_SYSTEM_FOREGROUND,
      EVENT_SYSTEM_FOREGROUND,
      NULL,
      WinEventProc,
      0,
      0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
   );

   // Hook for minimize start
   minimizeStartHook_ = SetWinEventHook (
      EVENT_SYSTEM_MINIMIZESTART,
      EVENT_SYSTEM_MINIMIZESTART,
      NULL,
      WinEventProc,
      0,
      0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
   );

   // Hook for minimize end
   minimizeEndHook_ = SetWinEventHook (
      EVENT_SYSTEM_MINIMIZEEND,
      EVENT_SYSTEM_MINIMIZEEND,
      NULL,
      WinEventProc,
      0,
      0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
   );

   if (!locationHook_ || !foregroundHook_ || !minimizeStartHook_ || !minimizeEndHook_) {
      Logger::log (Logger::Level::E_ERROR, "Failed to set one or more window event hooks");
      Shutdown ();
      return false;
   }

   initialized_ = true;
   Logger::log (Logger::Level::E_INFO, "Window event hooks initialized successfully");

   return true;
}

void WindowEventHook::Shutdown ()
{
   std::lock_guard<std::mutex> lock (mutex_);

   if (!initialized_) {
      return;
   }

   if (locationHook_) {
      UnhookWinEvent (locationHook_);
      locationHook_ = nullptr;
   }

   if (foregroundHook_) {
      UnhookWinEvent (foregroundHook_);
      foregroundHook_ = nullptr;
   }

   if (minimizeStartHook_) {
      UnhookWinEvent (minimizeStartHook_);
      minimizeStartHook_ = nullptr;
   }

   if (minimizeEndHook_) {
      UnhookWinEvent (minimizeEndHook_);
      minimizeEndHook_ = nullptr;
   }

   if (tsfn_) {
      tsfn_.Release ();
   }

   initialized_ = false;
   targetWindow_ = nullptr;

   Logger::log (Logger::Level::E_INFO, "Window event hooks shut down");
}

void WindowEventHook::SetTargetWindow (HWND hwnd)
{
   std::lock_guard<std::mutex> lock (mutex_);
   targetWindow_ = hwnd;

   if (hwnd) {
      Logger::log (Logger::Level::E_DEBUG, "Target window set for event tracking");
   } else {
      Logger::log (Logger::Level::E_DEBUG, "Target window cleared");
   }
}

HWND WindowEventHook::GetTargetWindow () const
{
   return targetWindow_;
}

void CALLBACK WindowEventHook::WinEventProc (
   HWINEVENTHOOK hook,
   DWORD event,
   HWND hwnd,
   LONG idObject,
   LONG idChild,
   DWORD dwEventThread,
   DWORD dwmsEventTime
)
{
   // Only process window events (not child objects)
   if (idObject != OBJID_WINDOW) {
      return;
   }

   WindowEventHook& instance = GetInstance ();
   instance.HandleWindowEvent (event, hwnd);
}

void WindowEventHook::HandleWindowEvent (DWORD event, HWND hwnd)
{
   // Special handling for foreground changes when we have a target window
   if (event == EVENT_SYSTEM_FOREGROUND && targetWindow_) {
      // Check if the game window lost focus
      if (hwnd != targetWindow_) {
         // The game is no longer the foreground window
         // Send a "focus lost" event for the game window
         WindowEventData data = {};
         data.handle = targetWindow_;

         // Get current bounds of the game window
         if (GetWindowRect (targetWindow_, &data.bounds)) {
            // Get monitor information
            HMONITOR monitor = MonitorFromWindow (targetWindow_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO monitorInfo = {};
            monitorInfo.cbSize = sizeof (MONITORINFO);

            if (GetMonitorInfo (monitor, &monitorInfo)) {
               data.monitor = monitorInfo.rcWork;
            }

            // Get DPI scaling
            UINT dpiX, dpiY;
            if (GetDpiForMonitor (monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) == S_OK) {
               data.scale = static_cast<double> (dpiX) / 96.0;
            } else {
               data.scale = 1.0;
            }

            // Check visibility
            data.visible = IsWindowVisible (targetWindow_) && !IsIconic (targetWindow_);
            data.focused = false; // Game lost focus

            TriggerCallback (data);
         }

         return;
      }
      // If hwnd == targetWindow_, fall through to normal processing
   }

   // If we have a target window, only process events for that window
   if (targetWindow_ && hwnd != targetWindow_) {
      return;
   }

   // Check if this is the game window
   wchar_t windowTitle [256];
   int length = GetWindowTextW (hwnd, windowTitle, 256);

   if (length <= 0) {
      return;
   }

   std::wstring title (windowTitle, length);

   // Check if it's "Dark and Darker  " (note the spaces)
   if (title.find (L"Dark and Darker") == std::wstring::npos) {
      // Not the game window, ignore
      return;
   }

   // Update target window if we found the game
   if (!targetWindow_) {
      std::lock_guard<std::mutex> lock (mutex_);
      targetWindow_ = hwnd;
   }

   // Get window information
   WindowEventData data = {};
   data.handle = hwnd;

   if (!GetWindowRect (hwnd, &data.bounds)) {
      return;
   }

   // Get monitor information
   HMONITOR monitor = MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST);
   MONITORINFO monitorInfo = {};
   monitorInfo.cbSize = sizeof (MONITORINFO);

   if (GetMonitorInfo (monitor, &monitorInfo)) {
      data.monitor = monitorInfo.rcWork;
   }

   // Get DPI scaling
   UINT dpiX, dpiY;
   if (GetDpiForMonitor (monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY) == S_OK) {
      data.scale = static_cast<double> (dpiX) / 96.0;
   } else {
      data.scale = 1.0;
   }

   // Check visibility
   data.visible = IsWindowVisible (hwnd) && !IsIconic (hwnd);

   // Check if focused
   data.focused = (GetForegroundWindow () == hwnd);

   TriggerCallback (data);
}

void WindowEventHook::TriggerCallback (const WindowEventData& data)
{
   if (!initialized_ || !tsfn_) {
      return;
   }

   // Allocate data on heap to pass to callback
   WindowEventData* heapData = new WindowEventData (data);

   // Call JavaScript callback from main thread
   auto status = tsfn_.NonBlockingCall (heapData, CallbackFromMainThread);

   if (status != napi_ok) {
      Logger::log (Logger::Level::E_ERROR, "Failed to queue window event callback");
      delete heapData;
   }
}

void WindowEventHook::CallbackFromMainThread (Napi::Env env, Napi::Function jsCallback, WindowEventData* data)
{
   if (!data) {
      return;
   }

   // Create JavaScript object with window data
   Napi::Object bounds = Napi::Object::New (env);
   bounds.Set ("x", Napi::Number::New (env, data->bounds.left));
   bounds.Set ("y", Napi::Number::New (env, data->bounds.top));
   bounds.Set ("width", Napi::Number::New (env, data->bounds.right - data->bounds.left));
   bounds.Set ("height", Napi::Number::New (env, data->bounds.bottom - data->bounds.top));

   Napi::Object monitor = Napi::Object::New (env);
   monitor.Set ("x", Napi::Number::New (env, data->monitor.left));
   monitor.Set ("y", Napi::Number::New (env, data->monitor.top));
   monitor.Set ("width", Napi::Number::New (env, data->monitor.right - data->monitor.left));
   monitor.Set ("height", Napi::Number::New (env, data->monitor.bottom - data->monitor.top));
   monitor.Set ("scale", Napi::Number::New (env, data->scale));

   Napi::Object result = Napi::Object::New (env);
   result.Set ("bounds", bounds);
   result.Set ("monitor", monitor);
   result.Set ("visible", Napi::Boolean::New (env, data->visible));
   result.Set ("focused", Napi::Boolean::New (env, data->focused));

   // Call the JavaScript callback
   jsCallback.Call ({ result });

   // Clean up heap data
   delete data;
}
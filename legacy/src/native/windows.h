#pragma once

#include <napi.h>
#include <windows.h>
#include <string>
#include <functional>
#include <mutex>

class ActiveWindowWorker : public Napi::AsyncWorker
{
   public:
      ActiveWindowWorker (const Napi::Env& Env);

      void Execute () override;
      void OnOK () override;
      void OnError (const Napi::Error &e) override;

      Napi::Promise GetPromise ();

   private:
      Napi::Promise::Deferred deferred;
      std::wstring title;
};

class GameWindowWorker : public Napi::AsyncWorker
{
   public:
      GameWindowWorker (const Napi::Env& Env);

      void Execute () override;
      void OnOK () override;
      void OnError (const Napi::Error &e) override;

      Napi::Promise GetPromise ();

   private:

      Napi::Promise::Deferred deferred;

      HWND handle = nullptr;

      RECT window;
      RECT monitor;

      double scale = 1.0;
};

// Window event data structure
struct WindowEventData
{
   HWND handle;
   RECT bounds;
   RECT monitor;
   double scale;
   bool visible;
   bool focused;
};

// Window event hook manager
class WindowEventHook
{
   public:
      static WindowEventHook& GetInstance ();

      bool Initialize (Napi::Env env, Napi::Function callback);
      void Shutdown ();

      void SetTargetWindow (HWND hwnd);
      HWND GetTargetWindow () const;

   private:
      WindowEventHook () = default;
      ~WindowEventHook ();

      WindowEventHook (const WindowEventHook&) = delete;
      WindowEventHook& operator= (const WindowEventHook&) = delete;

      static void CALLBACK WinEventProc (
         HWINEVENTHOOK hook,
         DWORD event,
         HWND hwnd,
         LONG idObject,
         LONG idChild,
         DWORD dwEventThread,
         DWORD dwmsEventTime
      );

      void HandleWindowEvent (DWORD event, HWND hwnd);
      void TriggerCallback (const WindowEventData& data);

      static void CallbackFromMainThread (Napi::Env env, Napi::Function jsCallback, WindowEventData* data);

      HWINEVENTHOOK locationHook_ = nullptr;
      HWINEVENTHOOK foregroundHook_ = nullptr;
      HWINEVENTHOOK minimizeStartHook_ = nullptr;
      HWINEVENTHOOK minimizeEndHook_ = nullptr;

      HWND targetWindow_ = nullptr;

      Napi::ThreadSafeFunction tsfn_;

      std::mutex mutex_;
      bool initialized_ = false;
};
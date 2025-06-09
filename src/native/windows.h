#pragma once

#include <napi.h>
#include <windows.h>
#include <string>

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
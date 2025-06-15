#include "async.cpp"
#include "screen.h"
#include "util.h"
#include "windows.h"
#include <napi.h>
#include <string>
#include <mutex>

std::shared_ptr<Screen> GlobalScreen = nullptr;
std::mutex GlobalScreenMutex;

Napi::Value Initialize (const Napi::CallbackInfo& Info) 
{
   Napi::Env Env = Info.Env ();
   Napi::HandleScope scope (Env);
   
   if (Info.Length () < 3) {
      Napi::TypeError::New (
         Env, 
         "Wrong number of arguments. Expected: tesseractPath, onnxFile, callback"
      ).ThrowAsJavaScriptException ();
      
      return Env.Null ();
   }
   
   if (!Info [0].IsString () || !Info [1].IsString ()) {
      Napi::TypeError::New (
         Env, 
         "Wrong arguments. Expected: string, string"
      ).ThrowAsJavaScriptException ();
      
      return Env.Null ();
   }
   
   std::string TesseractPath = Info [0].As<Napi::String> ().Utf8Value ();
   std::string OnnxFile = Info [1].As<Napi::String> ().Utf8Value ();
   
   {
      std::lock_guard<std::mutex> lock(GlobalScreenMutex);
      if (GlobalScreen) {
         GlobalScreen.reset ();
      }
      
      GlobalScreen = std::make_shared<Screen> ();
   }
   
   Screen::TesseractPath = TesseractPath;
   Screen::OnnxFile = OnnxFile;
   
   auto callback = Napi::ThreadSafeFunction::New (
      Env,
      Info [2].As<Napi::Function> (),
      "LogCallback",
      0,
      1
   );
   
   Logger::initialize (callback);
   
   bool Result = GlobalScreen->Initialize ();
   
   if (!Result) {
      std::lock_guard<std::mutex> lock(GlobalScreenMutex);
      GlobalScreen.reset ();
   }
   
   return Napi::Boolean::New (Env, Result);
}

Napi::Value GetTooltip (const Napi::CallbackInfo& Info) 
{
   Napi::Env Env = Info.Env ();
   Napi::HandleScope Scope (Env);
   
   try {
      std::shared_ptr<Screen> screen;
      {
         std::lock_guard<std::mutex> lock(GlobalScreenMutex);
         if (!GlobalScreen) {
            Napi::Error::New (Env, "Screen not initialized").ThrowAsJavaScriptException ();
            return Env.Undefined ();
         }
         screen = GlobalScreen;
      }
      
      auto* Worker = new TooltipWorker (Env, screen);
      Worker->Queue ();
      
      return Worker->GetPromise ();
   } catch (const std::exception& E) {
      Napi::Error::New (Env, std::string ("Exception in GetTooltip: ") + E.what ()).ThrowAsJavaScriptException ();
      return Env.Undefined ();
   } catch (...) {
      Napi::Error::New (Env, "Unknown exception in GetTooltip").ThrowAsJavaScriptException ();
      return Env.Undefined ();
   }
}

Napi::Value FetchActiveWindow (const Napi::CallbackInfo& Info) 
{
   auto* Worker = new ActiveWindowWorker (Info.Env ());
   Worker->Queue ();
   return Worker->GetPromise ();
}

Napi::Value FetchGameWindow (const Napi::CallbackInfo& Info) 
{
   auto* Worker = new GameWindowWorker (Info.Env ());
   Worker->Queue ();
   return Worker->GetPromise ();
}

Napi::Value Cleanup (const Napi::CallbackInfo& Info) 
{
   Napi::Env Env = Info.Env ();
   
   try {
      std::lock_guard<std::mutex> lock(GlobalScreenMutex);
      if (GlobalScreen) {
         GlobalScreen.reset ();
      }
      return Napi::Boolean::New (Env, true);
   } catch (const std::exception& E) {
      Napi::Error::New (Env, std::string ("Exception in Cleanup: ") + E.what ()).ThrowAsJavaScriptException ();
      return Env.Undefined ();
   }
}

Napi::Object Init (Napi::Env Env, Napi::Object Exports) 
{
   Exports.Set ("initialize", Napi::Function::New (Env, Initialize));
   Exports.Set ("getTooltip", Napi::Function::New (Env, GetTooltip));
   Exports.Set ("getActiveWindow", Napi::Function::New (Env, FetchActiveWindow));
   Exports.Set ("getGameWindow", Napi::Function::New (Env, FetchGameWindow));
   Exports.Set ("cleanup", Napi::Function::New (Env, Cleanup));
   
   return Exports;
}

NODE_API_MODULE (Screen, Init)
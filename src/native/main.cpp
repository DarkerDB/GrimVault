#include "async.cpp"
#include "screen.h"
#include "util.h"
#include <iostream>
#include <napi.h>
#include <optional>
#include <string>

Screen Screen;

Napi::Value Initialize (const Napi::CallbackInfo& Info) 
{
    Napi::Env Env = Info.Env ();
    Napi::HandleScope scope (Env);
    
    if (Info.Length () < 4) {
        Napi::TypeError::New (
            Env, 
            "Wrong number of arguments. Expected: tesseractPath, onnxFile, callback, captureMethod"
        ).ThrowAsJavaScriptException ();
        
        return Env.Null ();
    }

    if (!Info [0].IsString () || !Info [1].IsString ()) {
        Napi::TypeError::New (
            Env, 
            "Wrong arguments. Expected: string, string"
        ).ThrowAsJavaScriptException();
        
        return Env.Null ();
    }

    std::string TesseractPath = Info [0].As<Napi::String> ().Utf8Value ();
    std::string OnnxFile = Info [1].As<Napi::String> ().Utf8Value ();
    std::string CaptureMethod = Info [3].As<Napi::String> ().Utf8Value ();

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

    return Napi::Boolean::New (
        Env, 
        Screen.Initialize (CaptureMethod)
    );
}

Napi::Value GetTooltip (const Napi::CallbackInfo& Info) 
{
    Napi::Env Env = Info.Env ();
    Napi::HandleScope scope (Env);

    try {
        auto* Worker = new TooltipWorker (Env, &Screen);
        Worker->Queue ();

        return Worker->GetPromise ();
    } catch (const std::exception& e) {
        Napi::Error::New (Env, std::string ("Exception in GetTooltip: ") + e.what ()).ThrowAsJavaScriptException ();
        return Env.Undefined ();
    } catch (...) {
        Napi::Error::New (Env, "Unknown exception in GetTooltip").ThrowAsJavaScriptException ();
        return Env.Undefined ();
    }
}

Napi::Object Init (Napi::Env Env, Napi::Object Exports) 
{
    Exports.Set ("initialize", Napi::Function::New (Env, Initialize));
    Exports.Set ("getTooltip", Napi::Function::New (Env, GetTooltip));

    // Exports.Set ("startWindowEventListener", Napi::Function::New (Env, WindowEvents::StartWindowEventListener));
    // Exports.Set ("stopWindowEventListener", Napi::Function::New (Env, WindowEvents::StopWindowEventListener));

    return Exports;
}

NODE_API_MODULE (Screen, Init)
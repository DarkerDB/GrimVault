#include "logger.h"
#include "screen.h"
#include <napi.h>
#include <opencv2/core.hpp>
#include <optional>
#include <tesseract/baseapi.h>

class TooltipWorker : public Napi::AsyncWorker 
{
    public:
        TooltipWorker (const Napi::Env& env, Screen* screen) 
            : Napi::AsyncWorker (env), 
            deferred (Napi::Promise::Deferred::New (env)),
            screen (screen)
        {
        }

        ~TooltipWorker () 
        {
        }

        void Execute () override
        {
            try {
                tooltip = std::nullopt;

                // Logger::log (
                //     Logger::Level::E_DEBUG,
                //     "Attempting screen capture"
                // );

                std::optional<cv::Mat> maybeScreenshot = screen->Capture ();

                if (!maybeScreenshot) {
                    error = "Failed to capture the screen";
                    return;
                }

                cv::Mat screenshot = *maybeScreenshot;

                std::vector<cv::Rect> tooltips;

                try {
                    // Logger::log (
                    //     Logger::Level::E_DEBUG,
                    //     "Attempting to find tooltip in screenshot"
                    // );

                    std::optional<std::vector<cv::Rect>> maybeTooltips = screen->FindTooltips (screenshot);

                    if (!maybeTooltips) {
                        // Logger::log (
                        //     Logger::Level::E_DEBUG,
                        //     "No tooltip found"
                        // );

                        return;
                    }

                    tooltips = maybeTooltips.value ();
                } catch (const cv::Exception& e) {
                    error = std::string ("OpenCV error while finding tooltip: ") + e.what ();
                    return;
                }

                try {
                    // Logger::log (
                    //     Logger::Level::E_DEBUG,
                    //     "Attempting to run OCR against found tooltip"
                    // );

                    // text = screen->Read (screenshot (*tooltip));

                    // Until we retrain the tooltip model to not recognize the GrimVault tooltip, 
                    // we have to check the text to see if it is a valid tooltip.

                    for (const auto& candidate : tooltips) {
                        text = screen->Read (screenshot (candidate));
                        
                        // Logger::log (
                        //     Logger::Level::E_DEBUG,
                        //     "Found tooltip text: " + text
                        // );

                        if (text.find ("Item Statistics") == std::string::npos) {
                            tooltip = candidate;
                            break;
                        }
                    }
                    
                    if (!tooltip) {
                        error = std::string ("All identified tooltips belong to GrimVault");
                        return;
                    }
                } catch (const std::runtime_error& e) {
                    error = std::string ("Tesseract error while reading text: ") + e.what ();
                    return;
                } catch (const cv::Exception& e) {
                    error = std::string ("OpenCV error while processing region for OCR: ") + e.what ();
                    return;
                }
            } catch (const std::exception& e) {
                error = std::string ("Standard exception in TooltipWorker: ") + e.what ();
            } catch (const cv::Exception& e) {
                error = std::string ("OpenCV error in TooltipWorker: ") + e.what ();
            } catch (const std::runtime_error& e) {
                error = std::string ("Runtime error in TooltipWorker: ") + e.what ();
            } catch (...) {
                std::string exceptionTypeName;

                try {
                    exceptionTypeName = typeid (std::current_exception ()).name ();
                } catch (...) {
                    exceptionTypeName = "Unknown";
                }

                error = "Unknown exception in TooltipWorker (type: " + exceptionTypeName + ")";

                DWORD errorCode = GetLastError ();

                if (errorCode != 0) {
                    LPSTR messageBuffer = nullptr;
                
                    size_t size = FormatMessageA (
                        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS,
                        nullptr,
                        errorCode,
                        MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
                        (LPSTR) &messageBuffer,
                        0,
                        nullptr
                    );
                    
                    if (messageBuffer) {
                        error += "\nSystem error: " + std::string (messageBuffer, size);
                        LocalFree (messageBuffer);
                    }
                }
            }
        }

        void OnOK () override
        {
            Napi::Env env = Env ();

            if (!error.empty ()) {
                deferred.Reject (Napi::String::New (env, error));
                return;
            }

            if (!tooltip) {
                return deferred.Resolve (env.Null ());
            }

            // float scale = GetScalingFactorForMonitor ();
            float scale = 1;

            Napi::Object result = Napi::Object::New (env);

            result.Set ("text", Napi::String::New (env, text));

            result.Set ("x", Napi::Number::New (env, tooltip->x / scale));
            result.Set ("y", Napi::Number::New (env, tooltip->y / scale));
            result.Set ("width", Napi::Number::New (env, tooltip->width / scale));
            result.Set ("height", Napi::Number::New (env, tooltip->height / scale));

            deferred.Resolve (result);
        }

        void OnError (const Napi::Error& e) override
        {
            Logger::log (
                Logger::Level::E_ERROR,
                "Error in TooltipWorker: " + std::string (e.Message ())    
            );

            deferred.Reject (e.Value ());
        }

        Napi::Promise GetPromise () const
        {
            return deferred.Promise ();
        }

    private:
        Screen* screen;

        Napi::Promise::Deferred deferred;

        std::optional<cv::Rect> tooltip;

        std::string error;
        std::string text;
};
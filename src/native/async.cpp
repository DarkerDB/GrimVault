#include "logger.h"
#include "screen.h"
#include "util.h"
#include <napi.h>
#include <opencv2/core.hpp>
#include <optional>

class TooltipWorker : public Napi::AsyncWorker 
{
   public:

   TooltipWorker (const Napi::Env& Env, Screen* ScreenPtr) : Napi::AsyncWorker (Env), 
      Deferred (Napi::Promise::Deferred::New (Env)),
      ScreenObj (ScreenPtr),
      Screenshot (nullptr)
   {
   }
   
   ~TooltipWorker () 
   {
      if (Screenshot) {
         delete Screenshot;
         Screenshot = nullptr;
      }
   }
   
   void Execute () override
   {
      try {
         Tooltip = std::nullopt;

         // Logger::log (
         //     Logger::Level::E_DEBUG,
         //     "Attempting screen capture"
         // );
         
         std::optional<cv::Mat> MaybeScreenshot = ScreenObj->Capture ();
         
         if (!MaybeScreenshot) {
            Error = "Failed to capture the screen";
            return;
         }
         
         // Store screenshot in heap memory to avoid potential stack overflow
         Screenshot = new cv::Mat (*MaybeScreenshot);
         
         std::vector<cv::Rect> Tooltips;
         
         try {
            // Logger::log (
            //     Logger::Level::E_DEBUG,
            //     "Attempting to find tooltip in screenshot"
            // );
            
            std::optional<std::vector<cv::Rect>> MaybeTooltips = ScreenObj->FindTooltips (*Screenshot);
            
            if (!MaybeTooltips) {
               // Logger::log (
               //     Logger::Level::E_DEBUG,
               //     "No tooltip found"
               // );
               
               return;
            }
            
            Tooltips = MaybeTooltips.value ();
         } catch (const cv::Exception& E) {
            Error = std::string ("OpenCV error while finding tooltip: ") + E.what ();
            return;
         }
         
         try {
            // Logger::log (
            //     Logger::Level::E_DEBUG,
            //     "Attempting to run OCR against found tooltip"
            // );
            
            // Text = ScreenObj->Read (Screenshot (*Tooltip));
            
            // Until we retrain the tooltip model to not recognize the GrimVault tooltip, 
            // we have to check the text to see if it is a valid tooltip.
            
            for (const auto& Candidate : Tooltips) {
               Text = ScreenObj->Read ((*Screenshot) (Candidate));
               
               // Logger::log (
               //     Logger::Level::E_DEBUG,
               //     "Found tooltip text: " + Text
               // );
               
               if (Text.find ("Item Statistics") == std::string::npos) {
                  Tooltip = Candidate;
                  break;
               }
            }
            
            if (!Tooltip) {
               Error = std::string ("All identified tooltips belong to GrimVault");
               return;
            }
         } catch (const std::runtime_error& E) {
            Error = std::string ("Tesseract error while reading text: ") + E.what ();
            return;
         } catch (const cv::Exception& E) {
            Error = std::string ("OpenCV error while processing region for OCR: ") + E.what ();
            return;
         }
      } catch (const cv::Exception& E) {
         Error = std::string ("OpenCV error in TooltipWorker: ") + E.what ();
      } catch (const std::runtime_error& E) {
         Error = std::string ("Runtime error in TooltipWorker: ") + E.what ();
      } catch (const std::exception& E) {
         Error = std::string ("Standard exception in TooltipWorker: ") + E.what ();
      } catch (...) {
         std::string ExceptionTypeName;
         
         try {
            ExceptionTypeName = typeid (std::current_exception ()).name ();
         } catch (...) {
            ExceptionTypeName = "Unknown";
         }
         
         Error = "Unknown exception in TooltipWorker (type: " + ExceptionTypeName + ")";
         
         DWORD ErrorCode = GetLastError ();
         
         if (ErrorCode != 0) {
            LPSTR MessageBuffer = nullptr;
            
            size_t Size = FormatMessageA (
               FORMAT_MESSAGE_ALLOCATE_BUFFER | 
               FORMAT_MESSAGE_FROM_SYSTEM |
               FORMAT_MESSAGE_IGNORE_INSERTS,
               nullptr,
               ErrorCode,
               MAKELANGID (LANG_NEUTRAL, SUBLANG_DEFAULT),
               (LPSTR) &MessageBuffer,
               0,
               nullptr
            );
            
            if (MessageBuffer) {
               Error += "\nSystem error: " + std::string (MessageBuffer, Size);
               LocalFree (MessageBuffer);
            }
         }
      }
   }
   
   void OnOK () override
   {
      Napi::Env EnvLocal = Env ();
      
      if (!Error.empty ()) {
         Deferred.Reject (Napi::String::New (EnvLocal, Error));
         return;
      }
      
      if (!Tooltip) {
         return Deferred.Resolve (EnvLocal.Null ());
      }
      
      Napi::Object Result = Napi::Object::New (EnvLocal);
      
      Result.Set ("text", Napi::String::New (EnvLocal, Text));
      
      Result.Set ("x", Napi::Number::New (EnvLocal, Tooltip->x));
      Result.Set ("y", Napi::Number::New (EnvLocal, Tooltip->y));
      Result.Set ("width", Napi::Number::New (EnvLocal, Tooltip->width));
      Result.Set ("height", Napi::Number::New (EnvLocal, Tooltip->height));
      
      Deferred.Resolve (Result);
   }
   
   void OnError (const Napi::Error& E) override
   {
      Logger::log (
         Logger::Level::E_ERROR,
         "Error in TooltipWorker: " + std::string (E.Message ())    
      );
      
      Deferred.Reject (E.Value ());
   }
   
   Napi::Promise GetPromise () const
   {
      return Deferred.Promise ();
   }
   
   private:

   Screen* ScreenObj;
   
   Napi::Promise::Deferred Deferred;
   
   std::optional<cv::Rect> Tooltip;
   cv::Mat* Screenshot;
   
   std::string Error;
   std::string Text;
};
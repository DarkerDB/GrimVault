#pragma once

#include "wgc.h"
#include "ocr.h"
#include <atomic>
#include <mutex>
#include <opencv2/dnn.hpp>
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <vector>
#include <memory>
#include <chrono>
#include <functional>
#include <thread>
#include <windows.h>

// Forward declarations for screen capture lite
namespace SL {
   namespace Screen_Capture {
      struct Image;
      struct Monitor;
      class IScreenCaptureManager;
   }
}

class Screen 
{
   public:

   static std::string OCRModelPath;
   static std::string OCRDictPath;
   static std::string OnnxFile;
   static bool DebugMode;
   static std::string DebugPath;
   static std::atomic<int> DebugCounter;

   ~Screen ();
   Screen ();

   bool Initialize (const std::string& debugPath = "");
   std::optional<cv::Mat> Capture ();
   
   std::optional<std::vector<cv::Rect>> FindTooltips (cv::Mat Screenshot);
   std::string Read (cv::Mat Region);
   
   private:
   
   enum class CaptureMethod {
      ScreenCaptureLite,
      WindowsGraphicsCapture
   };

   const std::vector<std::string> MODEL_OBJECTS = { "Tooltip" };
   
   const float MODEL_WIDTH = 640;
   const float MODEL_HEIGHT = 640;
   
   const double MINIMUM_OBJECT_CONFIDENCE = 0.90;
   
   const double NMS_SCORE_THRESHOLD = 0.45;
   const double NMS_THRESHOLD = 0.50;
   
   std::atomic<bool> IsInitialized;
   std::thread::id MainThreadId;
   
   std::mutex CaptureLock;
   std::mutex DNNLock;

   std::unique_ptr<cv::dnn::Net> Net;
   std::unique_ptr<OCR> OcrEngine;
   
   // Capture method selection
   CaptureMethod CurrentCaptureMethod;
   
   // Screen capture lite implementation
   std::shared_ptr<SL::Screen_Capture::IScreenCaptureManager> CaptureManager;
   
   std::unique_ptr<WindowsGraphicsCapture> WGCInstance;
   
   // Common capture data
   cv::Mat LatestFrame;
   cv::Mat BackupFrame;  // Backup frame for when no new frame is available
   std::mutex FrameMutex;
   std::atomic<bool> HasNewFrame = false;
   
   bool InitializeScreenCaptureLite ();
   bool InitializeWindowsGraphicsCapture ();

   void Cleanup ();

   // Debug helper
   static void SaveDebugImage (const cv::Mat& image, const std::string& name);
};
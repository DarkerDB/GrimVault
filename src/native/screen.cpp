#include "logger.h"
#include "screen.h"
#include "util.h"
#include <chrono>
#include <dxgi1_6.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <thread>
#include <windows.h>
#include <wrl/client.h>

#define SC_LITE_STATIC
#include <ScreenCapture.h>

std::string Screen::TesseractPath = "";
std::string Screen::OnnxFile = "";

Screen::~Screen () 
{
   Cleanup ();
}

Screen::Screen () : IsInitialized (false), MainThreadId (std::this_thread::get_id ())
{
}

bool Screen::Initialize () 
{
   if (TesseractPath.empty () || OnnxFile.empty ()) {
      Logger::log (
         Logger::Level::E_ERROR,
         "Paths not set: TesseractPath and OnnxFile must be set before screen initialization"
      );
      
      return false;
   }

   Cleanup ();   

   Logger::log (
      Logger::Level::E_INFO, 
      "Initializing screen"
   );
   
   try {
      if (!Tesseract) {
         Logger::log (
            Logger::Level::E_INFO, 
            "Initializing Tesseract"
         );
         
         Tesseract = std::make_unique<tesseract::TessBaseAPI> ();
         
         if (Tesseract->Init (TesseractPath.c_str (), "eng", tesseract::OEM_LSTM_ONLY) != 0) {
            Logger::log (
               Logger::Level::E_ERROR,
               "Failed to initialize tesseract using datapath: " + TesseractPath
            );
            
            Cleanup ();
            return false;
         }
         
         Tesseract->SetPageSegMode (tesseract::PSM_SINGLE_BLOCK);
         Tesseract->SetVariable ("debug_file", "/dev/null");
         Tesseract->SetVariable ("user_defined_dpi", "70");
      }
      
      if (!Net) {
         Logger::log (
            Logger::Level::E_INFO, 
            "Initializing ONNX model"
         );

         Net = std::make_unique<cv::dnn::Net> (
            cv::dnn::readNetFromONNX (OnnxFile)
         );
         
         if (Net->empty ()) {
            Logger::log (
               Logger::Level::E_ERROR, 
               "Failed to load tooltip recognition model from: " + OnnxFile
            );
            
            Cleanup ();
            return false;
         }
         
         if (cv::cuda::getCudaEnabledDeviceCount () > 0) {
            Logger::log (
               Logger::Level::E_INFO,
               "CUDA is available, enabling GPU acceleration"
            );
            
            Net->setPreferableBackend (cv::dnn::DNN_BACKEND_CUDA);
            Net->setPreferableTarget (cv::dnn::DNN_TARGET_CUDA);
         } else {
            Logger::log (
               Logger::Level::E_INFO, 
               "CUDA not available, using CPU"
            );
            
            Net->setPreferableBackend (cv::dnn::DNN_BACKEND_OPENCV);
            Net->setPreferableTarget (cv::dnn::DNN_TARGET_CPU);
         }
      }

      Logger::log (
         Logger::Level::E_INFO,
         "Deciding on which capture method to use based on if there are any monitors running with HDR"
      );

      CurrentCaptureMethod = CaptureMethod::ScreenCaptureLite;

      struct MonitorEnumData {
         bool FoundHDR;
         HMONITOR HDRMonitor;
         Screen* ScreenInstance;
      };
      
      bool HasHDR = false;

      EnumDisplayMonitors (nullptr, nullptr, [] (HMONITOR Monitor, HDC, LPRECT, LPARAM LParam) -> BOOL {
         auto* HasHDR = reinterpret_cast <bool*> (LParam);

         if (IsMonitorHDR (Monitor)) {
            *HasHDR = true;
            return FALSE;
         }

         return TRUE;
      }, reinterpret_cast<LPARAM> (&HasHDR));

      if (HasHDR) {
         Logger::log (
            Logger::Level::E_INFO,
            "Found at least 1 monitor running HDR, using Windows Graphic Capture"
         );

         CurrentCaptureMethod = CaptureMethod::WindowsGraphicsCapture;
      }

      switch (CurrentCaptureMethod) {
         case CaptureMethod::WindowsGraphicsCapture:
            if (!InitializeWindowsGraphicsCapture ()) {
               Logger::log (
                  Logger::Level::E_WARNING, 
                  "Windows Graphics Capture not implemented"
               );
            }
         break;

         case CaptureMethod::ScreenCaptureLite:
            if (!InitializeScreenCaptureLite ()) {
               Logger::log (
                   Logger::Level::E_WARNING,
                   "Failed to initialize Screen Capture Lite"
               );
            }
         break;
      }

      IsInitialized = true;
      
      Logger::log (
         Logger::Level::E_INFO,
         "Screen successfully initialized with capture method: " + 
         std::to_string(static_cast<int>(CurrentCaptureMethod))
      );
      
      return true;
   } catch (std::exception& E) {
      Logger::log (
         Logger::Level::E_ERROR,
         std::string ("Exception during initialization: ") + E.what ()
      );
      
      Cleanup ();
      return false;
   } catch (...) {
      Logger::log (
         Logger::Level::E_ERROR,
         std::string ("Unknown exception during initialization")
      );
      
      Cleanup ();
      return false;
   }

   Logger::log (
      Logger::Level::E_INFO,
      "Screen successfully initialized"
   );
}

void Screen::Cleanup () 
{
   Logger::log (
      Logger::Level::E_INFO, 
      "Cleaning up all screen resources"
   );
   
   if (CaptureManager) {
      try {
         // Give callbacks time to complete before cleanup
         std::this_thread::sleep_for (std::chrono::milliseconds (100));
      } catch (...) {
      }
   }
   
   CaptureManager = nullptr;
   
   // WGCInstance cleanup handled by unique_ptr
   WGCInstance.reset();
   
   std::lock_guard<std::mutex> Lock (FrameMutex);
   LatestFrame = cv::Mat ();
   BackupFrame = cv::Mat ();
   HasNewFrame = false;
   
   if (Tesseract) {
      Tesseract->End ();
      Tesseract.reset ();
   }
   
   if (Net) {
      Net.reset ();
   }
   
   IsInitialized = false;
}

std::optional<cv::Mat> Screen::Capture () 
{
   if (!IsInitialized) {
      throw std::runtime_error ("Cannot capture screen before initialization");
   }
   
   Logger::log (
      Logger::Level::E_DEBUG,
      "Capture called, method: " + std::to_string (static_cast<int>(CurrentCaptureMethod))
   );
   
   std::lock_guard<std::mutex> Lock (CaptureLock);
   
   if (CurrentCaptureMethod == CaptureMethod::WindowsGraphicsCapture && WGCInstance) {
      HWND GameWindow = FindGameWindow ();

      if (!GameWindow) {
         Logger::log (
             Logger::Level::E_DEBUG,
             "Failed to find game window for capture"
         );

         return std::nullopt;
      }

      std::optional<cv::Mat> frame = WGCInstance->CaptureWindow (GameWindow);
      
      if (!frame) {
         Logger::log (
            Logger::Level::E_WARNING,
            "Windows Graphics Capture failed to capture frame"
         );
        
         return std::nullopt;
      }
      
      return frame;
   }
   
   std::lock_guard<std::mutex> FrameLock (FrameMutex);
   
   // Logger::log (
   //    Logger::Level::E_DEBUG,
   //    "ScreenCaptureLite - HasNewFrame: " + std::to_string(HasNewFrame) + 
   //    ", LatestFrame empty: " + std::to_string (LatestFrame.empty ()) +
   //    ", BackupFrame empty: " + std::to_string (BackupFrame.empty ())
   // );
   
   if (!HasNewFrame || LatestFrame.empty ()) {
      // Check if we have a backup frame to use
      if (!BackupFrame.empty ()) {
         Logger::log (
            Logger::Level::E_DEBUG, 
            "No new frame available, using backup frame"
         );
         return BackupFrame.clone ();
      }
      
      Logger::log (
         Logger::Level::E_DEBUG, 
         "No new available frame has been buffered for capture and no backup frame exists"
      );
         
      return std::nullopt;
   }

   // Save current frame as backup before returning
   BackupFrame = LatestFrame.clone ();
   HasNewFrame = false;
   return LatestFrame.clone ();
}

std::optional<std::vector<cv::Rect>> Screen::FindTooltips (cv::Mat Screenshot) 
{
   if (!IsInitialized) {
      throw std::runtime_error ("Cannot find tooltip before initialization");
   }
   
   std::lock_guard<std::mutex> Lock (DNNLock);
   
   if (Screenshot.channels () == 4) {
      cv::cvtColor (Screenshot, Screenshot, cv::COLOR_BGRA2BGR);
   }
   
   int Max = std::max (Screenshot.cols, Screenshot.rows);
   
   cv::Mat Resized;
   
   Resized = cv::Mat::zeros (Max, Max, CV_8UC3);
   Screenshot.copyTo (Resized (cv::Rect (0, 0, Screenshot.cols, Screenshot.rows)));
   
   cv::Mat Frame;
   
   cv::dnn::blobFromImage (
      Resized, 
      Frame, 
      1 / 255.0,
      cv::Size (MODEL_WIDTH, MODEL_HEIGHT), 
      cv::Scalar (),
      true, 
      false
   );
   
   Net->setInput (Frame);
   
   std::vector<cv::Mat> Outputs;
   
   Net->forward (Outputs, Net->getUnconnectedOutLayersNames ());
   
   // -- -- //
   
   int Rows = Outputs [0].size [2];
   int Dimensions = Outputs [0].size [1];
   
   Outputs [0] = Outputs [0].reshape (1, Dimensions);
   cv::transpose (Outputs [0], Outputs [0]);
   
   float *Data = (float *) Outputs [0].data;
   
   float XScale = (float) Resized.cols / MODEL_WIDTH;
   float YScale = (float) Resized.rows / MODEL_HEIGHT;
   
   std::vector<int> ClassIds;
   std::vector<float> Confidences;
   std::vector<cv::Rect> Boxes;
   
   for (int i = 0; i < Rows; ++i) {
      float *ClassesScores = Data + 4;
      
      cv::Mat Scores (1, MODEL_OBJECTS.size (), CV_32FC1, ClassesScores);
      cv::Point ClassId;
      
      double MaxClassScore;
      
      cv::minMaxLoc (Scores, 0, &MaxClassScore, 0, &ClassId);
      
      if (MaxClassScore > MINIMUM_OBJECT_CONFIDENCE) {
         Confidences.push_back (MaxClassScore);
         ClassIds.push_back (ClassId.x);
         
         float X = Data [0];
         float Y = Data [1];
         float W = Data [2];
         float H = Data [3];
         
         int Left = (int) ((X - 0.5 * W) * XScale);
         int Top = (int) ((Y - 0.5 * H) * YScale);
         
         int Width = (int) (W * XScale);
         int Height = (int) (H * YScale);
         
         Boxes.push_back (cv::Rect (Left, Top, Width, Height));
      }
      
      Data += Dimensions;
   }
   
   // Non-maximum supression to remove redundant boxes.
   std::vector<int> Nms;
   
   cv::dnn::NMSBoxes (
      Boxes, 
      Confidences, 
      NMS_SCORE_THRESHOLD, 
      NMS_THRESHOLD,
      Nms
   );
   
   if (Nms.size () <= 0) {
      return std::nullopt;
   }
   
   std::vector<cv::Rect> Tooltips;
   
   for (int Idx : Nms) {
      Tooltips.push_back (Boxes [Idx]);
   }
   
   return Tooltips;
}

std::string Screen::Read (cv::Mat Region) 
{
   if (!IsInitialized) {
      throw std::runtime_error ("Cannot run OCR before initialization");
   }
   
   std::lock_guard<std::mutex> Lock (TesseractLock);
   
   cv::Mat Processed = cv::Mat::zeros (
      Region.size (), 
      Region.type ()
   );
   
   // Alpha - contrast control (1.0 - 3.0)
   // Brightness control - 0 (0 - 100)
   Region.convertTo (Processed, -1, 2, 0);
   
   // Trim border
   int BorderSize = 5;
   
   cv::Rect Roi (
      BorderSize, 
      BorderSize, 
      Processed.cols - 2 * BorderSize,
      Processed.rows - 2 * BorderSize
   );
   
   Processed = Processed (Roi);
   
   cv::Mat Grayscale;
   cv::Mat Binary;
   cv::Mat Sharpened;
   
   cv::cvtColor (
      Processed, 
      Grayscale, 
      cv::COLOR_BGR2GRAY
   );
   
   cv::threshold (
      Grayscale, 
      Binary, 0, 255,
      cv::THRESH_BINARY_INV | cv::THRESH_OTSU
   );
   
   cv::bilateralFilter (Binary, Sharpened, 5, 75, 75);
   
   Tesseract->SetImage (
      Sharpened.data, 
      Sharpened.cols, 
      Sharpened.rows,
      Sharpened.channels (), 
      Sharpened.step
   );
   
   std::unique_ptr<char[]> Text (Tesseract->GetUTF8Text ());
   
   if (Text) {
      return std::string (Text.get ());
   } else {
      Logger::log (
         Logger::Level::E_ERROR,
         "Failed to extract text from image with Tesseract"
      );
   }
   
   return std::string ();
}

bool Screen::InitializeScreenCaptureLite () 
{
   Logger::log (
      Logger::Level::E_INFO,
      "Initializing Screen Capture Lite"
   );

   auto GetMonitorsCallback = [] () {
      auto Monitors = SL::Screen_Capture::GetMonitors ();
      
      for (const auto& M : Monitors) {
         Logger::log (
            Logger::Level::E_INFO,
            "Monitor " + std::to_string (M.Id) + 
            ": " + std::to_string (M.Width) + 
            "x" + std::to_string (M.Height) +
            " (Original: " + std::to_string (M.OriginalWidth) + "x" + std::to_string (M.OriginalHeight) + ") " +
            "at (" + std::to_string (M.OffsetX) + "," + std::to_string (M.OffsetY) + ") " +
            "Scaling: " + std::to_string (M.Scaling)
         );
      }

      return Monitors;
   };

   auto Config = SL::Screen_Capture::CreateCaptureConfiguration (GetMonitorsCallback);

   Config->onNewFrame ([ this ] (const SL::Screen_Capture::Image& Img, const SL::Screen_Capture::Monitor& Monitor) {
      // Logger::log (
      //    Logger::Level::E_DEBUG,
      //    "ScreenCaptureLite callback triggered for monitor " + std::to_string(Monitor.Id)
      // );
      
      std::optional<int> CurrentGameMonitorId = GetGameMonitorId ();

      if (!CurrentGameMonitorId.has_value () || CurrentGameMonitorId.value () == Monitor.Id) {
         int Height = SL::Screen_Capture::Height (Img);
         int Width = SL::Screen_Capture::Width (Img);
         
         int TargetWidth = Monitor.OriginalWidth;
         int TargetHeight = Monitor.OriginalHeight;
         
         // Logger::log (
         //    Logger::Level::E_DEBUG,
         //    "Processing frame: " + std::to_string (Width) + "x" + std::to_string (Height) +
         //    " from monitor " + std::to_string (Monitor.Id) +
         //    " (" + std::to_string (Monitor.Width) + "x" + std::to_string (Monitor.Height) + ")" +
         //    ", scaling factor: " + std::to_string (Monitor.Scaling)
         // );
         
         // 4 bytes per pixel (BGRA)
         std::vector<uint8_t> Buffer (Width * Height * 4); 
         
         SL::Screen_Capture::Extract (
            Img, 
            Buffer.data (), 
            Buffer.size ()
         );

         cv::Mat Frame (
            Height, 
            Width, 
            CV_8UC4, 
            Buffer.data ()
         );
         
         std::lock_guard<std::mutex> Lock (FrameMutex);
         LatestFrame = Frame.clone ();
         HasNewFrame = true;
      }
   });

   CaptureManager = Config->start_capturing ();
   
   if (!CaptureManager) {
      Logger::log (
         Logger::Level::E_ERROR,
         "Failed to start screen capture"
      );
      
      return false;
   }

   Logger::log (
      Logger::Level::E_INFO,
      "Screen capture manager started successfully"
   );

   // 100ms = 0.1s = 10 FPS
   CaptureManager->setFrameChangeInterval (std::chrono::milliseconds (100));
   
   Logger::log (
      Logger::Level::E_INFO,
      "Frame interval set to 100ms"
   );
   
   // Give the capture system time to start and capture the first frame
   std::this_thread::sleep_for (std::chrono::milliseconds (200));
   
   Logger::log (
      Logger::Level::E_INFO,
      "Screen Capture Lite initialization complete"
   );
   
   return true;
}

bool Screen::InitializeWindowsGraphicsCapture ()
{
   Logger::log (
      Logger::Level::E_INFO, 
      "Initializing Windows Graphics Capture"
   );
   
   WGCInstance = std::make_unique<WindowsGraphicsCapture> ();
   
   if (!WGCInstance->Initialize ()) {
      Logger::log (
         Logger::Level::E_ERROR, 
         "Failed to initialize WGC"
      );

      WGCInstance.reset ();
      return false;
   }
   
   CurrentCaptureMethod = CaptureMethod::WindowsGraphicsCapture;
   IsInitialized = true;
   return true;
}
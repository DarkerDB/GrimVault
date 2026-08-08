#include "ocr.h"
#include "logger.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <fstream>
#include <algorithm>
#include <numeric>
#include <filesystem>
#include <atomic>

// Forward declaration to access Screen's debug static members
class Screen {
public:
   static bool DebugMode;
   static std::string DebugPath;
   static std::atomic<int> DebugCounter;
};

OCR::OCR ()
{
   OrtEnv = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "PaddleOCR");
}

OCR::~OCR ()
{
}

bool OCR::Initialize (const std::string& modelPath, const std::string& dictPath)
{
   try {
      // Load character dictionary
      if (!LoadDictionary (dictPath)) {
         Logger::log (
            Logger::Level::E_ERROR,
            "Failed to load OCR dictionary from: " + dictPath
         );
         return false;
      }

      // Configure ONNX session
      SessionOptions = std::make_unique<Ort::SessionOptions> ();
      SessionOptions->SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);

      // Try CUDA first, fall back to CPU
      try {
         OrtCUDAProviderOptions cuda_options;
         cuda_options.device_id = 0;
         SessionOptions->AppendExecutionProvider_CUDA (cuda_options);
         Logger::log (
            Logger::Level::E_INFO,
            "OCR using CUDA acceleration"
         );
      } catch (...) {
         Logger::log (
            Logger::Level::E_INFO,
            "OCR using CPU"
         );
      }

      // Load ONNX model
      std::wstring modelPathW (modelPath.begin (), modelPath.end ());
      OrtSession = std::make_unique<Ort::Session> (*OrtEnv, modelPathW.c_str (), *SessionOptions);

      Logger::log (
         Logger::Level::E_INFO,
         "OCR engine initialized successfully"
      );

      return true;
   } catch (const Ort::Exception& e) {
      Logger::log (
         Logger::Level::E_ERROR,
         "ONNX Runtime error during OCR initialization: " + std::string (e.what ())
      );
      return false;
   } catch (const std::exception& e) {
      Logger::log (
         Logger::Level::E_ERROR,
         "Error during OCR initialization: " + std::string (e.what ())
      );
      return false;
   }
}

std::string OCR::Read (cv::Mat image)
{
   std::lock_guard<std::mutex> lock (InferenceLock);

   try {
      // Save input image for debugging
      SaveDebugImage (image, "ocr_2_input");

      // Preprocess image
      cv::Mat processed = Preprocess (image);

      // Create input tensor
      std::vector<int64_t> inputShape = { 1, 3, MODEL_HEIGHT, MODEL_WIDTH };
      size_t inputSize = 1 * 3 * MODEL_HEIGHT * MODEL_WIDTH;

      std::vector<float> inputData (processed.begin<float> (), processed.end<float> ());

      auto memoryInfo = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value inputTensor = Ort::Value::CreateTensor<float> (
         memoryInfo,
         inputData.data (),
         inputSize,
         inputShape.data (),
         inputShape.size ()
      );

      // Run inference
      const char* inputNames[] = { "x" };
      const char* outputNames[] = { "fetch_name_0" };

      auto outputTensors = OrtSession->Run (
         Ort::RunOptions { nullptr },
         inputNames,
         &inputTensor,
         1,
         outputNames,
         1
      );

      // Extract output
      float* outputData = outputTensors[0].GetTensorMutableData<float> ();
      auto shape = outputTensors[0].GetTensorTypeAndShapeInfo ().GetShape ();
      size_t outputSize = std::accumulate (
         shape.begin (),
         shape.end (),
         1LL,
         std::multiplies<int64_t> ()
      );

      std::vector<float> output (outputData, outputData + outputSize);

      // Postprocess and decode text
      return Postprocess (output, shape);
   } catch (const Ort::Exception& e) {
      Logger::log (
         Logger::Level::E_ERROR,
         "ONNX Runtime error during OCR inference: " + std::string (e.what ())
      );
      return std::string ();
   } catch (const std::exception& e) {
      Logger::log (
         Logger::Level::E_ERROR,
         "Error during OCR inference: " + std::string (e.what ())
      );
      return std::string ();
   }
}

cv::Mat OCR::Preprocess (cv::Mat input)
{
   // Log input image info
   Logger::log (
      Logger::Level::E_DEBUG,
      "Preprocess input: " + std::to_string (input.cols) + "x" + std::to_string (input.rows) +
      ", channels: " + std::to_string (input.channels ()) +
      ", type: " + std::to_string (input.type ())
   );

   // Convert BGRA to BGR if needed
   if (input.channels () == 4) {
      cv::cvtColor (input, input, cv::COLOR_BGRA2BGR);
   }

   // Resize to fixed model size (no aspect ratio preservation, like PaddleOCR)
   cv::Mat resized;
   cv::resize (input, resized, cv::Size (MODEL_WIDTH, MODEL_HEIGHT), 0, 0, cv::INTER_CUBIC);

   // Save resized image for debugging
   if (Screen::DebugMode) {
      SaveDebugImage (resized, "ocr_3_resized");
   }

   // Convert BGR to RGB
   cv::cvtColor (resized, resized, cv::COLOR_BGR2RGB);

   // Normalize to [0, 1] - matching PaddleOCR (NOT [-1, 1])
   resized.convertTo (resized, CV_32FC3, 1.0 / 255.0);

   // Log a few pixel values to verify normalization
   if (resized.rows > 0 && resized.cols > 0) {
      cv::Vec3f pixel = resized.at<cv::Vec3f> (resized.rows / 2, resized.cols / 2);
      Logger::log (
         Logger::Level::E_DEBUG,
         "Center pixel (RGB): [" + std::to_string (pixel[0]) + ", " +
         std::to_string (pixel[1]) + ", " + std::to_string (pixel[2]) + "]"
      );
   }

   // Convert HWC to CHW format
   std::vector<cv::Mat> channels (3);
   cv::split (resized, channels);

   // Create output in CHW format: [C, H, W]
   cv::Mat chw (1, 3 * MODEL_HEIGHT * MODEL_WIDTH, CV_32FC1);
   float* data = chw.ptr<float> ();

   for (int c = 0; c < 3; c++) {
      std::memcpy (
         data + c * MODEL_HEIGHT * MODEL_WIDTH,
         channels[c].data,
         MODEL_HEIGHT * MODEL_WIDTH * sizeof (float)
      );
   }

   return chw;
}

std::string OCR::Postprocess (const std::vector<float>& output, const std::vector<int64_t>& shape)
{
   // Output shape: [batch_size, timesteps, num_classes]
   // For batch_size=1: [1, timesteps, num_classes]

   if (shape.size () < 3) {
      Logger::log (
         Logger::Level::E_ERROR,
         "Invalid output shape from OCR model"
      );
      return std::string ();
   }

   int batchSize = static_cast<int> (shape[0]);
   int numTimesteps = static_cast<int> (shape[1]);
   int numClasses = static_cast<int> (shape[2]);

   // DEBUG: Log shapes and dictionary size
   Logger::log (
      Logger::Level::E_DEBUG,
      "OCR output shape: [" + std::to_string (batchSize) + ", " +
      std::to_string (numTimesteps) + ", " + std::to_string (numClasses) + "], " +
      "Dict size: " + std::to_string (CharDict.size ()) + ", " +
      "Blank index: " + std::to_string (numClasses - 1)
   );

   if (batchSize != 1) {
      Logger::log (
         Logger::Level::E_ERROR,
         "Expected batch size 1, got: " + std::to_string (batchSize)
      );
      return std::string ();
   }

   // CTC decoding: argmax each timestep, remove blanks and duplicates
   // In PaddleOCR, blank token is typically at last index (numClasses - 1)
   int blankIndex = numClasses - 1;

   std::string result;
   int lastIndex = -1;

   // DEBUG: Track predictions
   std::string debugPredictions;

   for (int t = 0; t < numTimesteps; t++) {
      int maxIndex = 0;
      float maxScore = output[t * numClasses];

      // Find argmax for this timestep
      for (int c = 1; c < numClasses; c++) {
         float score = output[t * numClasses + c];
         if (score > maxScore) {
            maxScore = score;
            maxIndex = c;
         }
      }

      // DEBUG: Log first 10 predictions
      if (t < 10) {
         debugPredictions += "[t" + std::to_string (t) + ":idx" + std::to_string (maxIndex) + "] ";
      }

      // Skip blank and consecutive duplicates
      if (maxIndex != blankIndex && maxIndex != lastIndex) {
         if (maxIndex < static_cast<int> (CharDict.size ())) {
            result += CharDict[maxIndex];
         } else {
            // DEBUG: Index out of bounds
            Logger::log (
               Logger::Level::E_WARNING,
               "Predicted index " + std::to_string (maxIndex) + " exceeds dict size " +
               std::to_string (CharDict.size ())
            );
         }
      }

      lastIndex = maxIndex;
   }

   Logger::log (
      Logger::Level::E_DEBUG,
      "First 10 predictions: " + debugPredictions
   );

   Logger::log (
      Logger::Level::E_DEBUG,
      "Decoded text: '" + result + "' (length: " + std::to_string (result.length ()) + ")"
   );

   return result;
}

bool OCR::LoadDictionary (const std::string& dictPath)
{
   std::ifstream file (dictPath);

   if (!file.is_open ()) {
      Logger::log (
         Logger::Level::E_ERROR,
         "Failed to open dictionary file: " + dictPath
      );
      return false;
   }

   CharDict.clear ();

   // Load dictionary as-is (blank token typically at end in PaddleOCR)
   std::string line;
   while (std::getline (file, line)) {
      // Remove any trailing whitespace/newlines
      line.erase (line.find_last_not_of (" \n\r\t") + 1);
      CharDict.push_back (line);
   }

   file.close ();

   Logger::log (
      Logger::Level::E_INFO,
      "Loaded " + std::to_string (CharDict.size ()) + " characters from dictionary"
   );

   // DEBUG: Log first few characters
   std::string firstChars;
   for (size_t i = 0; i < std::min (size_t(10), CharDict.size ()); i++) {
      firstChars += "[" + std::to_string (i) + ":'" + CharDict[i] + "'] ";
   }
   Logger::log (
      Logger::Level::E_DEBUG,
      "First 10 dict entries: " + firstChars
   );

   return true;
}

void OCR::SaveDebugImage (const cv::Mat& image, const std::string& name)
{
   if (!Screen::DebugMode || image.empty () || Screen::DebugPath.empty ()) {
      return;
   }

   try {
      // Create unique filename with counter
      int counter = Screen::DebugCounter.fetch_add (1);
      std::string filename = Screen::DebugPath + "/" + name + "_" + std::to_string (counter) + ".png";

      cv::imwrite (filename, image);

      Logger::log (
         Logger::Level::E_DEBUG,
         "Saved OCR debug image: " + filename
      );
   } catch (const std::exception& e) {
      Logger::log (
         Logger::Level::E_ERROR,
         std::string ("Failed to save OCR debug image: ") + e.what ()
      );
   }
}

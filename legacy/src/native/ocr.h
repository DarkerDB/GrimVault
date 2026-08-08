#pragma once

#include <opencv2/core/mat.hpp>
#include <onnxruntime_cxx_api.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>

class OCR
{
public:
   OCR ();
   ~OCR ();

   bool Initialize (const std::string& modelPath, const std::string& dictPath);
   std::string Read (cv::Mat image);

private:
   // ONNX Runtime objects
   std::unique_ptr<Ort::Env> OrtEnv;
   std::unique_ptr<Ort::Session> OrtSession;
   std::unique_ptr<Ort::SessionOptions> SessionOptions;

   // Character dictionary (index -> character)
   std::vector<std::string> CharDict;

   // Model parameters
   static constexpr int MODEL_HEIGHT = 48;
   static constexpr int MODEL_WIDTH = 320;

   // Thread safety
   std::mutex InferenceLock;

   // Helper methods
   cv::Mat Preprocess (cv::Mat input);
   std::string Postprocess (const std::vector<float>& output, const std::vector<int64_t>& shape);
   bool LoadDictionary (const std::string& dictPath);
   void SaveDebugImage (const cv::Mat& image, const std::string& name);
};

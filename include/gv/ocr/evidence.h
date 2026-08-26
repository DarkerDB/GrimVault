#pragma once

#include <gv/capture/frame.h>
#include <gv/vision/tooltip_detector.h>

#include <opencv2/core.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace gv::ocr {

struct EvidenceLine {
   cv::Mat image;
   std::size_t source_band = 0;
   bool title = false;
   std::string prediction;
   float confidence = 0.0f;
};

class Evidence
{
public:
   explicit Evidence (
      std::filesystem::path root = {},
      std::uintmax_t max_bytes = 250ull * 1024ull * 1024ull);

   bool enabled () const noexcept;
   void begin (
      std::uint64_t generation,
      const capture::Frame& frame,
      const cv::Mat& image,
      const std::vector<vision::TooltipBox>& boxes,
      const capture::Rect& selected,
      const cv::Mat& tooltip,
      const cv::Mat& identity,
      std::uint64_t identity_key);
   void observe (
      const capture::Frame& frame,
      const cv::Mat& image,
      const std::vector<vision::TooltipBox>& boxes,
      std::string reason);
   void ocr (
      std::uint64_t generation,
      const cv::Mat& tooltip,
      const std::vector<EvidenceLine>& lines,
      const std::string& prediction,
      float confidence);
   void snapshot (
      std::uint64_t generation,
      std::string name,
      const cv::Mat& image,
      const std::vector<vision::TooltipBox>& boxes);
   void event (
      std::uint64_t generation,
      std::string name,
      std::unordered_map<std::string, std::string> fields = {});

   static void trim (const std::filesystem::path& root, std::uintmax_t max_bytes);

private:
   std::filesystem::path root_;
   std::uintmax_t max_bytes_;
   std::mutex mutex_;
   std::unordered_map<std::uint64_t, std::filesystem::path> bundles_;
   std::chrono::steady_clock::time_point last_observation_;
};

}

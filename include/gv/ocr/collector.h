#pragma once

#include <gv/capture/frame.h>
#include <gv/ocr/evidence.h>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace gv::ocr {

class Collector
{
public:
   explicit Collector (std::filesystem::path root = {});

   bool enabled () const noexcept;
   bool save (
      std::uint64_t generation,
      std::string locale,
      std::uint64_t identity,
      const capture::Rect& rect,
      const cv::Mat& tooltip,
      const std::vector<EvidenceLine>& lines,
      const std::string& prediction,
      float confidence);

private:
   std::filesystem::path root_;
   std::mutex mutex_;
};

}

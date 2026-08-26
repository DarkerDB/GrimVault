#include <gv/ocr/evidence.h>

#include <gv/core/logger.h>

#include <opencv2/imgcodecs.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace gv::ocr {

namespace {

long long stamp ()
{
   return std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::system_clock::now ().time_since_epoch ()).count ();
}

nlohmann::json rect_json (const capture::Rect& rect)
{
   return {
      { "x", rect.x },
      { "y", rect.y },
      { "width", rect.w },
      { "height", rect.h },
   };
}

std::string hex (std::uint64_t value)
{
   std::ostringstream result;
   result << std::hex << std::setfill ('0') << std::setw (16) << value;
   return result.str ();
}

void append (
   const std::filesystem::path& path,
   std::uint64_t generation,
   std::string name,
   const std::unordered_map<std::string, std::string>& fields)
{
   nlohmann::json data = fields;
   nlohmann::json entry {
      { "schema", 1 },
      { "captured_unix_ms", stamp () },
      { "generation", generation },
      { "event", std::move (name) },
      { "data", std::move (data) },
   };
   std::ofstream output { path / "events.jsonl", std::ios::binary | std::ios::app };
   output << entry.dump () << '\n';
}

std::uintmax_t size_of (const std::filesystem::path& path)
{
   std::error_code ec;
   std::uintmax_t result = 0;
   for (std::filesystem::recursive_directory_iterator it { path, ec }, end;
        !ec && it != end; it.increment (ec)) {
      if (it->is_regular_file (ec)) result += it->file_size (ec);
   }
   return result;
}

}

Evidence::Evidence (std::filesystem::path root, std::uintmax_t max_bytes)
   : root_ (std::move (root)), max_bytes_ (max_bytes)
{}

bool Evidence::enabled () const noexcept { return !root_.empty (); }

void Evidence::begin (
   std::uint64_t generation,
   const capture::Frame& frame,
   const cv::Mat& image,
   const std::vector<vision::TooltipBox>& boxes,
   const capture::Rect& selected,
   const cv::Mat& tooltip,
   const cv::Mat& identity,
   std::uint64_t identity_key)
{
   if (!enabled () || image.empty () || tooltip.empty ()) return;
   std::lock_guard lock { mutex_ };
   try {
      std::error_code ec;
      std::filesystem::create_directories (root_, ec);
      const auto id = std::to_string (stamp ()) + "-g" + std::to_string (generation);
      const auto path = root_ / id;
      if (!std::filesystem::create_directory (path, ec)) return;
      bundles_ [generation] = path;

      cv::imwrite ((path / "frame.jpg").string (), image,
         { cv::IMWRITE_JPEG_QUALITY, 75 });
      cv::imwrite ((path / "tooltip.png").string (), tooltip);
      if (!identity.empty ()) cv::imwrite ((path / "identity.png").string (), identity);

      nlohmann::json detections = nlohmann::json::array ();
      for (const auto& box : boxes) {
         detections.push_back ({
            { "rect", rect_json (box.rect) },
            { "confidence", box.confidence },
            { "class_id", box.class_id },
         });
      }
      nlohmann::json manifest {
         { "schema", 1 },
         { "id", id },
         { "captured_unix_ms", stamp () },
         { "generation", generation },
         { "identity", hex (identity_key) },
         { "frame", {
            { "file", "frame.jpg" },
            { "width", frame.width },
            { "height", frame.height },
            { "backend", std::string { capture::backend_name (frame.backend) } },
            { "cursor", {
               { "x", frame.cursor.x },
               { "y", frame.cursor.y },
               { "valid", frame.cursor.valid },
            } },
         } },
         { "detections", std::move (detections) },
         { "selected", rect_json (selected) },
         { "tooltip_file", "tooltip.png" },
         { "identity_file", "identity.png" },
      };
      std::ofstream output { path / "manifest.json", std::ios::binary };
      output << manifest.dump (2) << '\n';
      append (path, generation, "accepted", {
         { "identity", hex (identity_key) },
         { "detections", std::to_string (boxes.size ()) },
      });
      trim (root_, max_bytes_);
      std::erase_if (bundles_, [] (const auto& value) {
         return !std::filesystem::exists (value.second);
      });
   } catch (const std::exception& error) {
      core::Logger::warn ("evidence begin failed: {}", error.what ());
   }
}

void Evidence::observe (
   const capture::Frame& frame,
   const cv::Mat& image,
   const std::vector<vision::TooltipBox>& boxes,
   std::string reason)
{
   if (!enabled () || image.empty ()) return;
   std::lock_guard lock { mutex_ };
   const auto now = std::chrono::steady_clock::now ();
   if (last_observation_.time_since_epoch ().count () != 0
       && now - last_observation_ < std::chrono::milliseconds { 250 }) return;
   last_observation_ = now;
   try {
      std::error_code ec;
      std::filesystem::create_directories (root_, ec);
      const auto id = std::to_string (stamp ()) + "-observation";
      const auto path = root_ / id;
      if (!std::filesystem::create_directory (path, ec)) return;

      cv::imwrite ((path / "frame.jpg").string (), image,
         { cv::IMWRITE_JPEG_QUALITY, 75 });
      nlohmann::json detections = nlohmann::json::array ();
      for (std::size_t index = 0; index < boxes.size (); ++index) {
         const auto file = "detection-" + (index < 10 ? std::string { "0" } : std::string {})
            + std::to_string (index) + ".png";
         cv::Rect crop {
            boxes [index].rect.x,
            boxes [index].rect.y,
            boxes [index].rect.w,
            boxes [index].rect.h,
         };
         crop &= cv::Rect { 0, 0, image.cols, image.rows };
         if (crop.area () > 0) cv::imwrite ((path / file).string (), image (crop));
         detections.push_back ({
            { "rect", rect_json (boxes [index].rect) },
            { "confidence", boxes [index].confidence },
            { "class_id", boxes [index].class_id },
            { "file", file },
         });
      }
      nlohmann::json manifest {
         { "schema", 1 },
         { "id", id },
         { "captured_unix_ms", stamp () },
         { "reason", std::move (reason) },
         { "frame", {
            { "file", "frame.jpg" },
            { "width", frame.width },
            { "height", frame.height },
            { "backend", std::string { capture::backend_name (frame.backend) } },
            { "cursor", {
               { "x", frame.cursor.x },
               { "y", frame.cursor.y },
               { "valid", frame.cursor.valid },
            } },
         } },
         { "detections", std::move (detections) },
      };
      std::ofstream output { path / "manifest.json", std::ios::binary };
      output << manifest.dump (2) << '\n';
      trim (root_, max_bytes_);
      std::erase_if (bundles_, [] (const auto& value) {
         return !std::filesystem::exists (value.second);
      });
   } catch (const std::exception& error) {
      core::Logger::warn ("evidence observation failed: {}", error.what ());
   }
}

void Evidence::ocr (
   std::uint64_t generation,
   const cv::Mat& tooltip,
   const std::vector<EvidenceLine>& lines,
   const std::string& prediction,
   float confidence)
{
   if (!enabled ()) return;
   std::lock_guard lock { mutex_ };
   const auto found = bundles_.find (generation);
   if (found == bundles_.end ()) return;
   try {
      if (!tooltip.empty ()) cv::imwrite ((found->second / "ocr-tooltip.png").string (), tooltip);
      nlohmann::json values = nlohmann::json::array ();
      for (std::size_t index = 0; index < lines.size (); ++index) {
         const auto file = "line-" + (index < 10 ? std::string { "0" } : std::string {})
            + std::to_string (index) + ".png";
         if (!lines [index].image.empty ())
            cv::imwrite ((found->second / file).string (), lines [index].image);
         values.push_back ({
            { "index", index },
            { "source_band", lines [index].source_band },
            { "title", lines [index].title },
            { "prediction", lines [index].prediction },
            { "confidence", lines [index].confidence },
            { "file", file },
         });
      }
      nlohmann::json entry {
         { "schema", 1 },
         { "captured_unix_ms", stamp () },
         { "generation", generation },
         { "event", "ocr" },
         { "data", {
            { "prediction", prediction },
            { "confidence", confidence },
            { "lines", std::move (values) },
         } },
      };
      std::ofstream output {
         found->second / "events.jsonl", std::ios::binary | std::ios::app };
      output << entry.dump () << '\n';
   } catch (const std::exception& error) {
      core::Logger::warn ("evidence OCR failed: {}", error.what ());
   }
}

void Evidence::snapshot (
   std::uint64_t generation,
   std::string name,
   const cv::Mat& image,
   const std::vector<vision::TooltipBox>& boxes)
{
   if (!enabled () || image.empty ()) return;
   std::lock_guard lock { mutex_ };
   const auto found = bundles_.find (generation);
   if (found == bundles_.end ()) return;
   try {
      const auto file = name + "-frame.jpg";
      cv::imwrite ((found->second / file).string (), image,
         { cv::IMWRITE_JPEG_QUALITY, 75 });
      nlohmann::json detections = nlohmann::json::array ();
      for (const auto& box : boxes) {
         detections.push_back ({
            { "rect", rect_json (box.rect) },
            { "confidence", box.confidence },
            { "class_id", box.class_id },
         });
      }
      nlohmann::json entry {
         { "schema", 1 },
         { "captured_unix_ms", stamp () },
         { "generation", generation },
         { "event", name },
         { "data", {
            { "frame_file", file },
            { "detections", std::move (detections) },
         } },
      };
      std::ofstream output {
         found->second / "events.jsonl", std::ios::binary | std::ios::app };
      output << entry.dump () << '\n';
   } catch (const std::exception& error) {
      core::Logger::warn ("evidence snapshot failed: {}", error.what ());
   }
}

void Evidence::event (
   std::uint64_t generation,
   std::string name,
   std::unordered_map<std::string, std::string> fields)
{
   if (!enabled ()) return;
   std::lock_guard lock { mutex_ };
   const auto found = bundles_.find (generation);
   if (found == bundles_.end ()) return;
   try {
      append (found->second, generation, std::move (name), fields);
   } catch (const std::exception& error) {
      core::Logger::warn ("evidence event failed: {}", error.what ());
   }
}

void Evidence::trim (const std::filesystem::path& root, std::uintmax_t max_bytes)
{
   if (root.empty () || max_bytes == 0) return;
   std::error_code ec;
   std::vector<std::filesystem::directory_entry> entries;
   std::uintmax_t total = 0;
   for (std::filesystem::directory_iterator it { root, ec }, end;
        !ec && it != end; it.increment (ec)) {
      if (!it->is_directory (ec)) continue;
      entries.push_back (*it);
      total += size_of (it->path ());
   }
   std::sort (entries.begin (), entries.end (), [] (const auto& left, const auto& right) {
      return left.last_write_time () < right.last_write_time ();
   });
   for (const auto& entry : entries) {
      if (total <= max_bytes) break;
      const auto bytes = size_of (entry.path ());
      std::filesystem::remove_all (entry.path (), ec);
      if (!ec) total = bytes > total ? 0 : total - bytes;
   }
}

}

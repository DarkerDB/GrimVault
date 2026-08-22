#include <gv/ocr/collector.h>

#include <gv/core/logger.h>
#include <gv/ocr/language.h>
#include <gv/ocr/preprocessor.h>

#include <opencv2/imgcodecs.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gv::ocr {

namespace {

std::string hex (std::uint64_t value)
{
   std::ostringstream result;
   result << std::hex << std::setfill ('0') << std::setw (16) << value;
   return result.str ();
}

std::string pixel_hash (const cv::Mat& image)
{
   std::uint64_t hash = 14695981039346656037ull;
   const auto mix = [&hash] (const void* data, std::size_t size) {
      const auto* bytes = static_cast<const unsigned char*> (data);
      for (std::size_t index = 0; index < size; ++index) {
         hash ^= bytes [index];
         hash *= 1099511628211ull;
      }
   };
   const int header[] { image.rows, image.cols, image.type () };
   mix (header, sizeof (header));
   for (int row = 0; row < image.rows; ++row)
      mix (image.ptr (row), static_cast<std::size_t> (image.cols) * image.elemSize ());
   return hex (hash);
}

void write_image (const std::filesystem::path& path, const cv::Mat& image)
{
   if (image.empty () || !cv::imwrite (path.string (), image))
      throw std::runtime_error { "image write failed: " + path.string () };
}

long long timestamp ()
{
   return std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::system_clock::now ().time_since_epoch ()).count ();
}

}

Collector::Collector (std::filesystem::path root) : root_ (std::move (root)) {}

bool Collector::enabled () const noexcept { return !root_.empty (); }

bool Collector::save (
   std::uint64_t generation,
   std::string locale,
   std::uint64_t identity,
   const capture::Rect& rect,
   const cv::Mat& tooltip,
   const std::vector<EvidenceLine>& lines,
   const std::string& prediction,
   float confidence)
{
   if (!enabled () || !preprocess::is_item_tooltip (tooltip, lines.size ())) return false;
   std::lock_guard lock { mutex_ };
   try {
      std::error_code error;
      std::filesystem::create_directories (root_, error);
      if (error) throw std::filesystem::filesystem_error { "create collector directory", root_, error };

      const auto hash = pixel_hash (tooltip);
      const auto family = family_of (locale);
      const auto id = std::string { family_dir (family) } + "-" + hash;
      const auto path = root_ / id;
      const auto metadata_path = path / "metadata.json";
      if (std::filesystem::exists (metadata_path)) return false;
      std::filesystem::create_directories (path, error);
      if (error) throw std::filesystem::filesystem_error { "create sample directory", path, error };

      write_image (path / "tooltip.png", tooltip);
      nlohmann::json line_values = nlohmann::json::array ();
      for (std::size_t index = 0; index < lines.size (); ++index) {
         const auto file = "line-" + (index < 10 ? std::string { "0" } : std::string {})
            + std::to_string (index) + ".png";
         write_image (path / file, lines [index].image);
         line_values.push_back ({
            { "index", index },
            { "source_band", lines [index].source_band },
            { "title", lines [index].title },
            { "prediction", lines [index].prediction },
            { "confidence", lines [index].confidence },
            { "file", file },
            { "width", lines [index].image.cols },
            { "height", lines [index].image.rows },
            { "pixel_hash", pixel_hash (lines [index].image) },
         });
      }

      nlohmann::json metadata {
         { "schema", 1 },
         { "id", id },
         { "captured_unix_ms", timestamp () },
         { "generation", generation },
         { "language", locale },
         { "family", std::string { family_dir (family) } },
         { "identity", hex (identity) },
         { "prediction", prediction },
         { "confidence", confidence },
         { "verified", false },
         { "tooltip_file", "tooltip.png" },
         { "tooltip_pixel_hash", hash },
         { "rect", {
            { "x", rect.x },
            { "y", rect.y },
            { "width", rect.w },
            { "height", rect.h },
         } },
         { "lines", std::move (line_values) },
      };
      const auto pending_path = path / "metadata.json.pending";
      {
         std::ofstream output { pending_path, std::ios::binary | std::ios::trunc };
         output << metadata.dump (2) << '\n';
         if (!output) throw std::runtime_error { "metadata write failed: " + pending_path.string () };
      }
      std::filesystem::rename (pending_path, metadata_path, error);
      if (error) throw std::filesystem::filesystem_error {
         "publish sample metadata", pending_path, metadata_path, error };
      core::Logger::info ("collector: saved {} lines={} locale={}", id, lines.size (), locale);
      return true;
   } catch (const std::exception& error) {
      core::Logger::warn ("collector: save failed: {}", error.what ());
      return false;
   }
}

}

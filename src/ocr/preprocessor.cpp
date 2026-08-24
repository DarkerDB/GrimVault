#include <gv/ocr/preprocessor.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>

namespace gv::ocr::preprocess {

namespace {
   constexpr int k_training_tooltip_max_background = 18;
   constexpr std::size_t k_training_tooltip_min_lines = 4;

   bool has_long_horizontal_run (const cv::Mat& row, int minimum)
   {
      int run = 0;
      for (int x = 0; x < row.cols; ++x) {
         if (row.at<std::uint8_t> (0, x)) {
            if (++run > minimum) return true;
         } else {
            run = 0;
         }
      }
      return false;
   }

   cv::Mat bright_mask (const cv::Mat& input)
   {
      cv::Mat gray, bright;
      if (input.channels () == 4)
         cv::cvtColor (input, gray, cv::COLOR_BGRA2GRAY);
      else if (input.channels () == 3)
         cv::cvtColor (input, gray, cv::COLOR_BGR2GRAY);
      else
         gray = input;
      if (input.channels () >= 3) {
         cv::Mat bgr;
         if (input.channels () == 4) cv::cvtColor (input, bgr, cv::COLOR_BGRA2BGR);
         else bgr = input;
         for (int y = 0; y < gray.rows; ++y) {
            auto* target = gray.ptr<std::uint8_t> (y);
            const auto* source = bgr.ptr<cv::Vec3b> (y);
            for (int x = 0; x < gray.cols; ++x) {
               const int maximum = std::max ({ source [x][0], source [x][1], source [x][2] });
               const int minimum = std::min ({ source [x][0], source [x][1], source [x][2] });
               if (maximum - minimum >= 32) target [x] = static_cast<std::uint8_t> (maximum);
            }
         }
      }
      // Tooltip labels are deliberately rendered in a very dark gray. A
      // fixed threshold of 80 retained colored values but erased labels such
      // as "Slot Type:". Estimate the dominant background luminance and keep
      // pixels sufficiently above it, with a ceiling that still handles
      // unusually bright capture backgrounds.
      int histogram [256] {};
      for (int y = 0; y < gray.rows; ++y) {
         const auto* row = gray.ptr<std::uint8_t> (y);
         for (int x = 0; x < gray.cols; ++x) ++histogram [row [x]];
      }
      const int midpoint = gray.rows * gray.cols / 2;
      int cumulative = 0, background = 0;
      for (; background < 255; ++background) {
         cumulative += histogram [background];
         if (cumulative >= midpoint) break;
      }
      const int threshold = std::clamp (background + 18, 24, 72);
      cv::threshold (gray, bright, threshold, 255, cv::THRESH_BINARY);
      return bright;
   }
}

std::vector<cv::Range> line_bands (const cv::Mat& crop)
{
   if (crop.empty () || crop.cols < 6) return {};
   const cv::Mat bright = bright_mask (crop);
   const int margin = std::min (bright.cols / 2 - 1,
      std::max (2, bright.cols / 25));
   if (margin < 0 || bright.cols - margin <= margin) return {};
   const cv::Mat interior = bright (
      cv::Range::all (), cv::Range (margin, bright.cols - margin));

   cv::Mat rowsum;
   cv::reduce (interior, rowsum, 1, cv::REDUCE_SUM, CV_32S);
   const int min_ink = interior.cols * 255 / 50;

   std::vector<cv::Range> bands;
   int top = -1, end = -1, gap = 0;
   auto flush = [&] {
      if (top >= 0 && end - top >= 6)
         bands.emplace_back (std::max (0, top - 2), std::min (crop.rows, end + 3));
      top = -1; end = -1; gap = 0;
   };

   for (int y = 0; y < rowsum.rows; ++y) {
      if (rowsum.at<int> (y) > min_ink) {
         if (top < 0) top = y;
         end = y;
         gap = 0;
      } else if (top >= 0 && ++gap > 3) {
         flush ();
      }
   }
   flush ();
   return bands;
}

std::size_t first_tooltip_band (const cv::Mat& crop, const std::vector<cv::Range>& bands)
{
   if (bands.size () < 2) return 0;
   const int limit = std::min (96, crop.rows / 4);
   for (std::size_t index = 0; index + 1 < bands.size (); ++index) {
      const auto& band = bands [index];
      if (band.end > limit) break;
      if (band.size () <= 24
          && is_horizontal_rule (crop (band, cv::Range::all ()))) return index + 1;
   }
   return 0;
}

std::optional<std::size_t> title_band (
   const cv::Mat& crop, const std::vector<cv::Range>& bands)
{
   for (std::size_t index = first_tooltip_band (crop, bands); index < bands.size (); ++index) {
      const auto& band = bands [index];
      const cv::Mat line = crop (band, cv::Range::all ());
      if (is_horizontal_rule (line) || band.size () < 18) continue;
      return index;
   }
   return std::nullopt;
}

bool top_is_clipped (const cv::Mat& crop, const std::vector<cv::Range>& bands)
{
   if (crop.empty ()) return false;
   const cv::Mat top = bright_mask (crop.rowRange (0, std::min (6, crop.rows)));
   const int left = top.cols / 3;
   const int right = top.cols * 2 / 3;
   if (right > left && cv::countNonZero (
         top (cv::Range::all (), cv::Range (left, right))) > 2) return true;
   if (bands.empty ()) return false;
   const auto first = first_tooltip_band (crop, bands);
   if (first >= bands.size ()) return false;
   const auto& band = bands [first];
   if (band.start > 3 || band.size () >= 18) return false;
   const cv::Mat mask = bright_mask (crop (band, cv::Range::all ()));
   return cv::countNonZero (mask (cv::Range::all (),
      cv::Range (mask.cols / 3, mask.cols * 2 / 3))) > 0;
}

cv::Mat trim_cols (const cv::Mat& line)
{
   if (line.empty ()) return line;
   cv::Mat colsum;
   cv::reduce (bright_mask (line), colsum, 0, cv::REDUCE_SUM, CV_32S);

   // Merge glyphs/words into coarse groups. Tiny groups separated from the
   // actual text by a large gap are tooltip ornaments (for example the two
   // dashes flanking a stat heading), not recognizer input.
   struct Group { int begin; int end; };
   std::vector<Group> groups;
   const int join_gap = std::max (4, line.rows / 2);
   int group_begin = -1, last_ink = -1;
   for (int x = 0; x < colsum.cols; ++x) {
      if (colsum.at<int> (x) > 0) {
         if (group_begin < 0) group_begin = x;
         else if (x - last_ink > join_gap) {
            groups.push_back ({ group_begin, last_ink + 1 });
            group_begin = x;
         }
         last_ink = x;
      }
   }
   if (group_begin >= 0) groups.push_back ({ group_begin, last_ink + 1 });
   if (groups.empty ()) return line;

   int x0 = groups.front ().begin;
   int x1 = groups.back ().end;
   if (groups.size () > 1) {
      const int min_content_width = std::max (6, line.rows);
      auto first = std::find_if (groups.begin (), groups.end (), [&] (const Group& g) {
         return g.end - g.begin >= min_content_width;
      });
      auto last = std::find_if (groups.rbegin (), groups.rend (), [&] (const Group& g) {
         return g.end - g.begin >= min_content_width;
      });
      if (first != groups.end () && last != groups.rend ()) {
         x0 = first->begin;
         x1 = last->end;
      }
   }
   x0 = std::max (0, x0 - 4);
   x1 = std::min (line.cols, x1 + 4);
   return line (cv::Range::all (), cv::Range (x0, x1));
}

std::vector<cv::Range> col_chunks (const cv::Mat& line)
{
   if (line.empty ()) return {};
   // English uses a 960x48 export and can retain an entire tooltip row. Other
   // families still use 320x48; the pipeline bypasses chunking for English.
   const int max_w = line.rows * 13 / 2;
   if (line.cols <= max_w) return { cv::Range (0, line.cols) };

   cv::Mat colsum;
   cv::reduce (bright_mask (line), colsum, 0, cv::REDUCE_SUM, CV_32S);
   std::vector<cv::Range> chunks;
   int start = 0;
   while (line.cols - start > max_w) {
      const int target = start + max_w;
      const int floor  = start + max_w * 3 / 5;
      int best_begin = -1, best_end = -1;
      // Three empty columns is often just the enclosed/diagonal geometry of a
      // glyph at this scale. A real inter-word space grows with line height.
      const int min_word_gap = std::max (5, line.rows / 5);
      int gap_end = target;
      while (gap_end > floor) {
         while (gap_end > floor && colsum.at<int> (gap_end - 1) > 0) --gap_end;
         const int end = gap_end;
         while (gap_end > floor && colsum.at<int> (gap_end - 1) == 0) --gap_end;
         if (end - gap_end >= min_word_gap) {
            best_begin = gap_end;
            best_end = end;
            break;
         }
      }
      const int cut = best_begin >= 0 ? (best_begin + best_end) / 2 : target;
      chunks.emplace_back (start, cut);
      start = cut;
   }
   if (start < line.cols) chunks.emplace_back (start, line.cols);
   return chunks;
}

cv::Mat trim_title_rule (const cv::Mat& line)
{
   if (line.empty () || line.rows < 8) return line;
   const cv::Mat mask = bright_mask (line);
   for (int y = 4; y < mask.rows; ++y) {
      if (!has_long_horizontal_run (mask.row (y), mask.cols / 3)) continue;
      int cut = y;
      while (cut > 0 && cv::countNonZero (mask.row (cut - 1)) > mask.cols / 8) --cut;
      if (cut >= 6) return line.rowRange (0, cut);
   }
   return line;
}

bool is_horizontal_rule (const cv::Mat& line)
{
   if (line.empty () || line.cols < line.rows * 8) return false;
   const cv::Mat mask = bright_mask (line);
   for (int y = 0; y < mask.rows; ++y) {
      if (has_long_horizontal_run (mask.row (y), mask.cols / 3)) return true;
   }
   return false;
}

bool is_item_tooltip (const cv::Mat& crop, std::size_t line_count)
{
   if (crop.empty () || line_count < k_training_tooltip_min_lines) return false;
   cv::Mat gray;
   if (crop.channels () == 4)
      cv::cvtColor (crop, gray, cv::COLOR_BGRA2GRAY);
   else if (crop.channels () == 3)
      cv::cvtColor (crop, gray, cv::COLOR_BGR2GRAY);
   else
      gray = crop;

   int histogram [256] {};
   for (int y = 0; y < gray.rows; ++y) {
      const auto* row = gray.ptr<std::uint8_t> (y);
      for (int x = 0; x < gray.cols; ++x) ++histogram [row [x]];
   }
   const int midpoint = gray.rows * gray.cols / 2;
   int cumulative = 0, background = 0;
   for (; background < 255; ++background) {
      cumulative += histogram [background];
      if (cumulative >= midpoint) break;
   }
   if (background > k_training_tooltip_max_background) return false;

   const cv::Mat mask = bright_mask (crop);
   for (int y = 0; y < mask.rows; ++y)
      if (has_long_horizontal_run (mask.row (y), mask.cols / 2)) return true;
   return false;
}

} // namespace gv::ocr::preprocess

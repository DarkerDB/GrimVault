#include <gv/core/environment.h>
#include <gv/core/logger.h>
#include <gv/ocr/capture_policy.h>
#include <gv/ocr/evidence.h>
#include <gv/ocr/preprocessor.h>
#include <gv/ocr/tooltip_state.h>
#include <gv/vision/gem_detector.h>
#include <gv/vision/tooltip_tracker.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <thread>

#include "pipeline_impl.h"

namespace gv::ocr {

namespace {

void replace_all (std::string& text, std::string_view from, std::string_view to)
{
   for (std::size_t pos = 0; (pos = text.find (from, pos)) != std::string::npos;) {
      text.replace (pos, from.size (), to);
      pos += to.size ();
   }
}

void canonicalize_latin (std::string& text)
{
   constexpr std::pair<std::string_view, std::string_view> replacements[] = {
      { "á", "a" }, { "à", "a" }, { "â", "a" }, { "ä", "a" }, { "é", "e" }, { "è", "e" },
      { "ê", "e" }, { "ë", "e" }, { "í", "i" }, { "ì", "i" }, { "î", "i" }, { "ï", "i" },
      { "ó", "o" }, { "ò", "o" }, { "ô", "o" }, { "ö", "o" }, { "ú", "u" }, { "ù", "u" },
      { "û", "u" }, { "ü", "u" }, { "−", "-" }, { "–", "-" }, { "’", "'" },
   };
   for (const auto& [from, to] : replacements) replace_all (text, from, to);
}

}

void Pipeline::Impl::ocr_loop ()
{
   struct Cached {
      TooltipObservation observation;
      LanguageFamily family = LanguageFamily::English;
      std::string text;
      std::string rarity;
      std::unordered_map<std::string, std::string> gems;
      float confidence = 0.0f;
   };
   std::vector<Cached> cache;
   std::uint64_t last_completed_generation = 0;
   while (running.load (std::memory_order_relaxed)) {
      std::optional<VisionOut> next;
      {
         std::lock_guard lock { vision_lock };
         next = std::move (pending_vision);
         pending_vision.reset ();
      }
      if (!next.has_value ()) {
         std::this_thread::sleep_for (std::chrono::milliseconds (5));
         continue;
      }

      VisionOut item = std::move (*next);

      if (item.generation != generation.load (std::memory_order_relaxed) ||
          item.generation == last_completed_generation)
         continue;

      if (!item.frame.data || item.frame.empty ()) {
         core::Logger::warn ("pipeline: dropping frame with no pixel data");
         continue;
      }

      LanguageFamily family;
      std::string locale_name;
      {
         std::lock_guard lock { language_lock };
         family = language;
         locale_name = locale;
      }
      const auto identity_key = item.observation.identity.key ();
      if (!item.refresh) {
         const auto found = std::find_if (cache.begin (), cache.end (), [&] (const Cached& value) {
            return value.family == family &&
                   value.observation.cacheable (item.observation, config.identity_bits,
                                                config.identity_size_px);
         });
         if (found != cache.end ()) {
            evidence.event (item.generation, "ocr_cache_hit",
                            {
                               { "identity", std::to_string (identity_key) },
                            });
            core::log::ocr.event ("cache_hit",
                                  {
                                     { "generation", std::to_string (item.generation) },
                                     { "identity", std::to_string (identity_key) },
                                  });
            last_completed_generation = item.generation;
            if (callback) {
               try {
                  callback (RecognizedTooltip {
                     .generation = item.generation,
                     .rect = item.boxes.front ().rect,
                     .text = found->text,
                     .rarity = found->rarity,
                     .gems = found->gems,
                     .confidence = found->confidence,
                     .backend = item.frame.backend,
                     .preliminary = false,
                     .captured_at = item.frame.timestamp,
                  });
               } catch (const std::exception& error) {
                  core::Logger::error ("pipeline: tooltip callback threw: {}", error.what ());
               }
            }
            continue;
         }
      }

      evidence.event (item.generation, "ocr_started",
                      {
                         { "identity", std::to_string (identity_key) },
                      });
      auto rec = registry.acquire (family);
      if (!rec.has_value ()) {
         core::Logger::error ("pipeline: language acquire failed: {}", rec.error ().message);
         continue;
      }

      cv::Mat bgra { item.frame.height, item.frame.width, CV_8UC4, item.frame.data.get (),
                     static_cast<std::size_t> (item.frame.stride) };

      for (const auto& box : item.boxes) {
         cv::Rect cv_box { box.rect.x, box.rect.y, box.rect.w, box.rect.h };
         cv_box &= cv::Rect (0, 0, bgra.cols, bgra.rows);

         if (cv_box.area () <= 0) continue;
         const cv::Rect sample_box = cv_box;

         auto make_crop = [&] {
            cv::Mat value = bgra (cv_box).clone ();
            if (value.cols > 24 && value.rows > 24)
               value = value (cv::Rect (6, 6, value.cols - 12, value.rows - 12));
            return value;
         };

         cv::Mat crop = make_crop ();
         auto bands = preprocess::line_bands (crop);
         if (preprocess::top_is_clipped (crop, bands) && cv_box.y > 0) {
            const int extension = std::min (64, cv_box.y);
            cv_box.y -= extension;
            cv_box.height += extension;
            crop = make_crop ();
            bands = preprocess::line_bands (crop);
         }

         const auto t0 = std::chrono::steady_clock::now ();
         const auto title_band = preprocess::title_band (crop, bands);
         const auto rarity = preprocess::tooltip_rarity (crop, bands);
         const auto segmented_at = std::chrono::steady_clock::now ();

         static const bool dump_bands = [] {
            const auto value = core::environment::get ("GRIMVAULT_OCR_DEBUG");
            return !value.empty () && value != "0";
         }();

         int dump_index = -1;
         std::filesystem::path dump_dir;
         if (dump_bands) {
            static std::atomic<int> seq { 0 };
            dump_index = seq++;
            dump_dir = std::filesystem::temp_directory_path () / "grimvault-ocr";
            std::error_code ec;
            std::filesystem::create_directories (dump_dir, ec);
            cv::imwrite ((dump_dir / (std::to_string (dump_index) + "_crop.png")).string (), crop);
            int b = 0;
            for (const auto& band : bands) {
               cv::imwrite ((dump_dir / (std::to_string (dump_index) + "_band" +
                                         std::to_string (b++) + ".png"))
                               .string (),
                            preprocess::trim_cols (crop (band, cv::Range::all ())));
            }
         }

         std::string text;
         std::unordered_map<std::string, std::string> gems;
         float conf_sum = 0.0f;
         int conf_n = 0;
         bool preliminary_sent = false;
         std::vector<EvidenceLine> evidence_lines;

         std::size_t source_band_index = 0;
         for (const auto& band : bands) {
            const auto source_index = source_band_index++;
            if (item.generation != generation.load (std::memory_order_relaxed)) break;
            if (!title_band.has_value () || source_index < *title_band) continue;
            cv::Mat raw_line = crop (band, cv::Range::all ());
            const bool is_rule = preprocess::is_horizontal_rule (raw_line);
            const bool is_title = source_index == *title_band;
            if (!is_title && is_rule) continue;
            if (is_title) raw_line = preprocess::trim_title_rule (raw_line);
            cv::Mat line = preprocess::trim_cols (raw_line);
            if (dump_index >= 0) {
               cv::imwrite ((dump_dir / (std::to_string (dump_index) + "_input" +
                                         std::to_string (source_index) + ".png"))
                               .string (),
                            line);
            }

            std::string line_text;
            float line_confidence = 0.0f;
            int line_confidence_n = 0;
            const std::vector<cv::Range> whole_line { cv::Range { 0, line.cols } };
            if (is_title && (*rec)->has_title_model ()) {
               auto full = (*rec)->read (line, true);
               if (full.has_value ()) {
                  line_text = full->text;
                  line_confidence = full->confidence;
                  line_confidence_n = 1;
                  conf_sum += full->confidence;
                  ++conf_n;
               }
            } else {
               const auto chunks = (*rec)->is_wide () ? whole_line : preprocess::col_chunks (line);
               for (const auto& chunk : chunks) {
                  if (item.generation != generation.load (std::memory_order_relaxed)) break;
                  auto res = (*rec)->read (line (cv::Range::all (), chunk));
                  if (!res.has_value () || res->text.empty ()) continue;

                  if (!line_text.empty ()) line_text.push_back (' ');
                  line_text += res->text;
                  conf_sum += res->confidence;
                  ++conf_n;
                  line_confidence += res->confidence;
                  ++line_confidence_n;
               }
            }

            if (line_confidence_n > 1) line_confidence /= line_confidence_n;
            if (family == LanguageFamily::Latin || family == LanguageFamily::French)
               canonicalize_latin (line_text);
            if (evidence.enabled ()) {
               evidence_lines.push_back (EvidenceLine {
                  .image = line.clone (),
                  .source_band = source_index,
                  .title = is_title,
                  .prediction = line_text,
                  .confidence = line_confidence,
               });
            }
            if (line_text.empty ()) continue;
            if (!is_title && std::any_of (line_text.begin (), line_text.end (), [] (char ch) {
                   return std::isdigit (static_cast<unsigned char> (ch)) != 0;
                })) {
               if (auto gem_family = vision::detect_gem_family (raw_line);
                   gem_family.has_value ()) {
                  gems[line_text] = *gem_family;
               }
            }
            if (!text.empty ()) text.push_back ('\n');
            text += line_text;

            if (!preliminary_sent && is_title && line_text.size () >= 2 &&
                line_confidence >= 0.65f &&
                item.generation == generation.load (std::memory_order_relaxed) && callback) {
               preliminary_sent = true;
               core::log::ocr.event ("title_ready",
                                     {
                                        { "generation", std::to_string (item.generation) },
                                        { "confidence", fmt::format ("{:.3f}", line_confidence) },
                                        { "text", line_text },
                                     });
               try {
                  callback (RecognizedTooltip {
                     .generation = item.generation,
                     .rect = box.rect,
                     .text = line_text,
                     .rarity = rarity.value_or (""),
                     .confidence = line_confidence,
                     .backend = item.frame.backend,
                     .preliminary = true,
                     .captured_at = item.frame.timestamp,
                  });
               } catch (const std::exception& e) {
                  core::Logger::error ("pipeline: preliminary callback threw: {}", e.what ());
               }
            }
         }

         const auto ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                            std::chrono::steady_clock::now () - t0)
                            .count ();
         const auto segment_us =
            std::chrono::duration_cast<std::chrono::microseconds> (segmented_at - t0).count ();

         core::Logger::info ("pipeline: timings detect={}ms ocr={}ms lines={}/{}",
                             last_detect_ms.load (), ms, conf_n, bands.size ());
         core::log::ocr.event (
            "recognition",
            {
               { "generation", std::to_string (item.generation) },
               { "family", std::string { family_dir (family) } },
               { "segment_us", std::to_string (segment_us) },
               { "total_ms", std::to_string (ms) },
               { "lines", std::to_string (conf_n) },
               { "bands", std::to_string (bands.size ()) },
               { "rarity", rarity.value_or ("unknown") },
               { "confidence", fmt::format ("{:.3f}", conf_n ? conf_sum / conf_n : 0.0f) },
            });

         const float confidence = conf_n ? conf_sum / conf_n : 0.0f;
         evidence.ocr (item.generation, crop, evidence_lines, text, confidence);
         const capture::Rect sample_rect { sample_box.x, sample_box.y, sample_box.width,
                                           sample_box.height };
         if (sample_cb) {
            try {
               sample_cb (TooltipSample {
                  .generation = item.generation,
                  .rect = sample_rect,
                  .image = bgra (sample_box).clone (),
                  .locale = locale_name,
                  .text = text,
                  .rarity = rarity.value_or (""),
                  .confidence = confidence,
                  .backend = item.frame.backend,
               });
            } catch (const std::exception& error) {
               core::Logger::warn ("collection: tooltip sample failed: {}", error.what ());
            }
         }
         if (item.generation != generation.load (std::memory_order_relaxed)) continue;
         if (text.empty ()) continue;
         last_completed_generation = item.generation;

         core::Logger::info (
            "OCR result generation={} family={} rarity={} confidence={:.3f} chars={}",
            item.generation, family_dir (family), rarity.value_or ("unknown"), confidence,
            text.size ());

         if (cache.size () == 128) cache.erase (cache.begin ());
         cache.push_back (Cached {
            .observation = item.observation,
            .family = family,
            .text = text,
            .rarity = rarity.value_or (""),
            .gems = gems,
            .confidence = confidence,
         });

         if (callback) {
            try {
               callback (RecognizedTooltip {
                  .generation = item.generation,
                  .rect = box.rect,
                  .text = std::move (text),
                  .rarity = rarity.value_or (""),
                  .gems = std::move (gems),
                  .confidence = confidence,
                  .backend = item.frame.backend,
                  .preliminary = false,
                  .captured_at = item.frame.timestamp,
               });
            } catch (const std::exception& e) {
               core::Logger::error ("pipeline: tooltip callback threw: {}", e.what ());
            }
         }
      }
   }
}

}

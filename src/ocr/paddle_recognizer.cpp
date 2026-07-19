#include <gv/ocr/paddle_recognizer.h>
#include <gv/core/logger.h>

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <fstream>
#include <cmath>
#include <mutex>

namespace gv::ocr {

namespace {

   // PaddleOCR v5 mobile_rec uses 48x320 CRNN input by default. The recognizer
   // is letterboxed to height 48 keeping aspect ratio, then padded to width 320.
   //
   // Inference runs on OpenCV DNN — the same stack as the tooltip detector.
   // ONNX Runtime is deliberately NOT used: vcpkg's onnxruntime build ships a
   // broken operator-schema registry in this dependency mix ("0 schema were
   // exposed ... 741 were expected"), so every Ort::Session fails to load any
   // model. One inference stack also means one set of DLLs to stage.
   constexpr int k_model_height       = 48;
   constexpr int k_default_model_width = 320;
   constexpr int k_english_model_width = 960;

} // namespace

struct PaddleRecognizer::Impl
{
   cv::dnn::Net             net;
   cv::dnn::Net             title_net;
   bool                     loaded = false;
   bool                     title_loaded = false;
   std::vector<std::string> dict;
   LanguageFamily           family = LanguageFamily::Latin;
   int                      model_width = k_default_model_width;
   std::mutex               lock;
};

PaddleRecognizer::PaddleRecognizer ()  : impl_ (std::make_unique<Impl> ()) {}
PaddleRecognizer::~PaddleRecognizer () = default;

LanguageFamily PaddleRecognizer::family () const noexcept                  { return impl_->family; }
bool PaddleRecognizer::has_title_model () const noexcept                   { return impl_->title_loaded; }
void           PaddleRecognizer::set_family (LanguageFamily f) noexcept
{
   impl_->family = f;
   impl_->model_width = f == LanguageFamily::English
      ? k_english_model_width : k_default_model_width;
}

core::Result<void> PaddleRecognizer::initialize (
   const std::filesystem::path& model_path,
   const std::filesystem::path& dict_path
) {
   try {
      impl_->net = cv::dnn::readNetFromONNX (model_path.string ());
      impl_->net.setPreferableBackend (cv::dnn::DNN_BACKEND_OPENCV);
      impl_->net.setPreferableTarget  (cv::dnn::DNN_TARGET_CPU);
      impl_->loaded = true;
      impl_->title_loaded = false;

      const auto title_path = model_path.parent_path () / "rec_title.onnx";
      if (impl_->family == LanguageFamily::English
          && std::filesystem::exists (title_path)) {
         impl_->title_net = cv::dnn::readNetFromONNX (title_path.string ());
         impl_->title_net.setPreferableBackend (cv::dnn::DNN_BACKEND_OPENCV);
         impl_->title_net.setPreferableTarget  (cv::dnn::DNN_TARGET_CPU);
         impl_->title_loaded = true;
      }

      std::ifstream df { dict_path };
      if (!df) {
         return core::fail (core::Error::make (core::ErrorKind::Io,
            "paddle_rec: failed to open dict {}", dict_path.string ()));
      }

      impl_->dict.clear ();
      std::string line;
      while (std::getline (df, line)) {
         line.erase (line.find_last_not_of (" \r\n\t") + 1);
         impl_->dict.push_back (line);
      }

      core::Logger::info ("paddle_rec: loaded model {} ({} chars in dict)",
         model_path.filename ().string (), impl_->dict.size ());
      if (impl_->title_loaded) {
         core::Logger::info ("paddle_rec: loaded title model rec_title.onnx");
      }

      return {};
   } catch (const cv::Exception& e) {
      impl_->loaded = false;
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: init failed: {}", e.what ()));
   }
}

namespace {

   // Letterbox to 48 high keeping aspect, pad right to 320 wide, then
   // normalize per PaddleOCR rec convention: (x/255 - 0.5) / 0.5 → [-1, 1].
   // blobFromImage expresses that as scale 1/127.5 with mean 127.5, and
   // handles the BGR→RGB swap + HWC→NCHW transpose.
   cv::Mat preprocess (const cv::Mat& input, int model_width)
   {
      cv::Mat bgr;
      if      (input.channels () == 4) cv::cvtColor (input, bgr, cv::COLOR_BGRA2BGR);
      else if (input.channels () == 3) bgr = input;
      else                              cv::cvtColor (input, bgr, cv::COLOR_GRAY2BGR);

      // Tooltip copy ranges from bright white down to very dark gray/brown on
      // a nearly black textured panel. Segmentation can still find the latter,
      // but the recognizer occasionally returns an empty row. Normalize each
      // already-isolated line by its own background and foreground percentiles
      // and lift antialiasing with a gentle gamma curve. OCR does not need the
      // semantic text color, only the glyph shape.
      cv::Mat gray;
      cv::cvtColor (bgr, gray, cv::COLOR_BGR2GRAY);
      int histogram [256] {};
      for (int y = 0; y < gray.rows; ++y) {
         const auto* row = gray.ptr<std::uint8_t> (y);
         for (int x = 0; x < gray.cols; ++x) ++histogram [row [x]];
      }
      const int pixels = gray.rows * gray.cols;
      auto percentile = [&] (int numerator, int denominator) {
         const int target = pixels * numerator / denominator;
         int cumulative = 0;
         for (int value = 0; value < 256; ++value) {
            cumulative += histogram [value];
            if (cumulative >= target) return value;
         }
         return 255;
      };
      const int background = percentile (1, 2);
      const int foreground = percentile (49, 50);
      const int span = std::max (24, foreground - background);
      cv::Mat lut (1, 256, CV_8U);
      for (int value = 0; value < 256; ++value) {
         const double normalized = std::clamp (
            static_cast<double> (value - background) / span, 0.0, 1.0);
         lut.at<std::uint8_t> (value) = static_cast<std::uint8_t> (
            std::lround (255.0 * std::sqrt (normalized)));
      }
      cv::LUT (gray, lut, gray);
      cv::cvtColor (gray, bgr, cv::COLOR_GRAY2BGR);

      const double scale = static_cast<double> (k_model_height) / bgr.rows;
      const int    w     = std::min (model_width,
         std::max (1, static_cast<int> (std::lround (bgr.cols * scale))));

      cv::Mat resized;
      cv::resize (bgr, resized, cv::Size (w, k_model_height), 0, 0, cv::INTER_CUBIC);

      cv::Mat canvas = cv::Mat::zeros (k_model_height, model_width, resized.type ());
      resized.copyTo (canvas (cv::Rect (0, 0, w, k_model_height)));

      return cv::dnn::blobFromImage (
         canvas,
         1.0 / 127.5,
         cv::Size (model_width, k_model_height),
         cv::Scalar (127.5, 127.5, 127.5),
         /*swapRB=*/ true,
         /*crop=*/   false
      );
   }

   // Greedy CTC decode: argmax per timestep, drop blank + consecutive dupes.
   std::pair<std::string, float> ctc_decode (
      const float*                    out,
      int                             timesteps,
      int                             classes,
      const std::vector<std::string>& dict
   ) {

      // PaddleOCR CTCLabelDecode convention (stock PP-OCR exports): class 0
      // is the CTC blank, classes 1..N map to dict lines 0..N-1, and a head
      // exported with use_space_char carries a literal space as the final
      // class (classes == dict + 2).
      constexpr int blank = 0;

      std::string result;
      int         last  = -1;
      double      conf_sum = 0;
      int         conf_n   = 0;

      for (int t = 0; t < timesteps; ++t) {
         int   best_i = 0;
         float best_v = out [t * classes];

         for (int c = 1; c < classes; ++c) {
            float v = out [t * classes + c];
            if (v > best_v) { best_v = v; best_i = c; }
         }

         if (best_i != blank && best_i != last) {
            const int di = best_i - 1;

            if (di >= 0 && di < static_cast<int> (dict.size ())) {
               result += dict [di];
               conf_sum += best_v;
               ++conf_n;
            } else if (best_i == classes - 1) {
               result += ' ';
               conf_sum += best_v;
               ++conf_n;
            }
         }

         last = best_i;
      }

      const float confidence = conf_n ? static_cast<float> (conf_sum / conf_n) : 0.0f;
      return { result, confidence };
   }

} // namespace

core::Result<RecognizerResult> PaddleRecognizer::read (const cv::Mat& line, bool title)
{
   if (!impl_->loaded) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: not initialized"));
   }

   std::lock_guard lock { impl_->lock };

   try {
      auto& net = title && impl_->title_loaded ? impl_->title_net : impl_->net;
      net.setInput (preprocess (line, impl_->model_width));
      cv::Mat out = net.forward ();

      // Output is [1, timesteps, classes].
      if (out.dims != 3 || out.size [0] != 1) {
         return core::fail (core::Error::make (core::ErrorKind::Ocr,
            "paddle_rec: unexpected output rank {} from model", out.dims));
      }

      auto [text, conf] = ctc_decode (
         out.ptr<float> (), out.size [1], out.size [2], impl_->dict);

      return RecognizerResult { .text = std::move (text), .confidence = conf };
   } catch (const cv::Exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: inference failed: {}", e.what ()));
   }
}

} // namespace gv::ocr
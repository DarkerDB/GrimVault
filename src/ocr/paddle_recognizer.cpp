#include <gv/ocr/paddle_recognizer.h>
#include <gv/core/logger.h>

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>

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
   // Inference runs on ONNX Runtime with the DirectML provider, the same
   // stack as the tooltip detector, falling back to CPU exactly as it does.
   //
   // This used to be OpenCV DNN because vcpkg's onnxruntime shipped a broken
   // operator-schema registry ("0 schema were exposed ... 741 were expected")
   // and no Ort::Session could load a model. cmake/OnnxRuntime.cmake sidesteps
   // that by fetching Microsoft's own DirectML build, so the constraint is
   // gone — and recognition, not detection, is what actually costs CPU here:
   // a median hover spends ~54 ms in this function against ~6 ms detecting.
   constexpr int k_model_height       = 48;
   constexpr int k_default_model_width = 320;

   core::Result<std::vector<std::string>> read_dictionary (const std::filesystem::path& path)
   {
      std::ifstream input { path };
      if (!input) {
         return core::fail (core::Error::make (core::ErrorKind::Io,
            "paddle_rec: failed to open dict {}", path.string ()));
      }
      std::vector<std::string> values;
      std::string line;
      while (std::getline (input, line)) {
         line.erase (line.find_last_not_of (" \r\n\t") + 1);
         values.push_back (line);
      }
      return values;
   }

} // namespace

// One loaded recognizer graph. Input geometry is fixed by the export
// (1x3x48xW); `run` hands OpenCV's NCHW blob straight to ORT, which wants the
// same contiguous float layout blobFromImage already produces.
struct Session
{
   std::unique_ptr<Ort::Session> session;
   std::string input_name;
   std::string output_name;
   bool        directml = false;
   int         width = k_default_model_width;
   int         classes = 0;

   explicit operator bool () const noexcept { return session != nullptr; }
};

struct PaddleRecognizer::Impl
{
   Ort::Env                 env { ORT_LOGGING_LEVEL_WARNING, "grimvault-ocr" };
   Session                  net;
   Session                  title_net;
   std::filesystem::path    model_path;
   bool                     loaded = false;
   bool                     title_loaded = false;
   std::vector<std::string> dict;
   std::vector<std::string> title_dict;
   LanguageFamily           family = LanguageFamily::Latin;
   int                      model_width = k_default_model_width;
   std::mutex               lock;

   Session load (const std::filesystem::path& path, bool gpu)
   {
      Ort::SessionOptions options;
      options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
      if (gpu) {
         options.DisableMemPattern ();
         options.SetExecutionMode (ExecutionMode::ORT_SEQUENTIAL);
         const OrtDmlApi* api = nullptr;
         Ort::ThrowOnError (Ort::GetApi ().GetExecutionProviderApi (
            "DML", ORT_API_VERSION, reinterpret_cast<const void**> (&api)));
         OrtDmlDeviceOptions device {
            .Preference = OrtDmlPerformancePreference::HighPerformance,
            .Filter = OrtDmlDeviceFilter::Gpu,
         };
         Ort::ThrowOnError (api->SessionOptionsAppendExecutionProvider_DML2 (options, &device));
      }

      Session out;
      out.session = std::make_unique<Ort::Session> (env, path.c_str (), options);
      Ort::AllocatorWithDefaultOptions allocator;
      out.input_name  = out.session->GetInputNameAllocated (0, allocator).get ();
      out.output_name = out.session->GetOutputNameAllocated (0, allocator).get ();
      const auto shape = out.session->GetInputTypeInfo (0).GetTensorTypeAndShapeInfo ().GetShape ();
      if (shape.size () == 4 && shape [3] > 0) out.width = static_cast<int> (shape [3]);
      const auto output_shape = out.session->GetOutputTypeInfo (0).GetTensorTypeAndShapeInfo ().GetShape ();
      if (output_shape.size () == 3 && output_shape [2] > 0) {
         out.classes = static_cast<int> (output_shape [2]);
      }
      out.directml    = gpu;
      return out;
   }

   // ORT wants the blob's floats as-is: blobFromImage already returns a
   // contiguous NCHW buffer of exactly the shape the export declares.
   cv::Mat run (Session& model, const cv::Mat& blob)
   {
      const std::array<std::int64_t, 4> shape {
         1, blob.size [1], blob.size [2], blob.size [3] };
      auto memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
      auto tensor = Ort::Value::CreateTensor<float> (
         memory, const_cast<float*> (blob.ptr<float> ()),
         static_cast<std::size_t> (blob.total ()), shape.data (), shape.size ());

      const std::array<const char*, 1> inputs  { model.input_name.c_str () };
      const std::array<const char*, 1> outputs { model.output_name.c_str () };

      auto values = model.session->Run (
         Ort::RunOptions { nullptr }, inputs.data (), &tensor, 1, outputs.data (), 1);

      const auto dims = values [0].GetTensorTypeAndShapeInfo ().GetShape ();
      if (dims.size () != 3) return {};

      // Copied, not wrapped: the Ort::Value owning this buffer dies here.
      const std::array<int, 3> sizes {
         static_cast<int> (dims [0]), static_cast<int> (dims [1]),
         static_cast<int> (dims [2]) };
      return cv::Mat (3, sizes.data (), CV_32F,
                      const_cast<float*> (values [0].GetTensorData<float> ())).clone ();
   }
};

PaddleRecognizer::PaddleRecognizer ()  : impl_ (std::make_unique<Impl> ()) {}
PaddleRecognizer::~PaddleRecognizer () = default;

LanguageFamily PaddleRecognizer::family () const noexcept                  { return impl_->family; }
bool PaddleRecognizer::has_title_model () const noexcept                   { return impl_->title_loaded; }
bool PaddleRecognizer::is_wide () const noexcept                           { return impl_->model_width > k_default_model_width; }
void           PaddleRecognizer::set_family (LanguageFamily f) noexcept
{
   impl_->family = f;
}

core::Result<void> PaddleRecognizer::initialize (
   const std::filesystem::path& model_path,
   const std::filesystem::path& dict_path
) {
   try {
      impl_->model_path = model_path;
      try {
         impl_->net = impl_->load (model_path, true);
      } catch (const Ort::Exception& error) {
         core::Logger::warn ("paddle_rec: DirectML unavailable: {}; using CPU", error.what ());
         impl_->net = impl_->load (model_path, false);
      }
      impl_->loaded = true;
      impl_->model_width = impl_->net.width;
      impl_->title_loaded = false;

      const auto title_path = model_path.parent_path () / model_files::rec_tooltip_title;
      if (model_path.filename () == model_files::rec_tooltip_body
          && std::filesystem::exists (title_path)) {
         try {
            impl_->title_net = impl_->load (title_path, impl_->net.directml);
            impl_->title_loaded = true;
         } catch (const Ort::Exception& error) {
            core::Logger::warn ("paddle_rec: title model unusable: {}", error.what ());
         }
      }

      auto body_dict = read_dictionary (dict_path);
      if (!body_dict) return core::fail (body_dict.error ());
      impl_->dict = std::move (*body_dict);
      if (impl_->net.classes != static_cast<int> (impl_->dict.size ()) + 2) {
         return core::fail (core::Error::make (core::ErrorKind::Ocr,
            "paddle_rec: model has {} classes but dictionary has {} characters",
            impl_->net.classes, impl_->dict.size ()));
      }

      impl_->title_dict = impl_->dict;
      const auto title_dict_path = model_path.parent_path () / model_files::rec_tooltip_title_dict;
      if (impl_->title_loaded && std::filesystem::exists (title_dict_path)) {
         auto title_dict = read_dictionary (title_dict_path);
         if (!title_dict) return core::fail (title_dict.error ());
         impl_->title_dict = std::move (*title_dict);
      }
      if (impl_->title_loaded
          && impl_->title_net.classes != static_cast<int> (impl_->title_dict.size ()) + 2) {
         return core::fail (core::Error::make (core::ErrorKind::Ocr,
            "paddle_rec: title model has {} classes but dictionary has {} characters",
            impl_->title_net.classes, impl_->title_dict.size ()));
      }

      core::Logger::info ("paddle_rec: loaded model {} on {} ({} chars in dict)",
         model_path.filename ().string (),
         impl_->net.directml ? "DirectML" : "CPU", impl_->dict.size ());
      if (impl_->title_loaded) {
         core::Logger::info ("paddle_rec: loaded title model {} ({} chars in dict)",
            model_files::rec_tooltip_title, impl_->title_dict.size ());
      }

      return {};
   } catch (const cv::Exception& e) {
      impl_->loaded = false;
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: init failed: {}", e.what ()));
   } catch (const Ort::Exception& e) {
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
      const bool use_title = title && impl_->title_loaded;
      auto& net = use_title ? impl_->title_net : impl_->net;
      const cv::Mat blob = preprocess (line, impl_->model_width);

      cv::Mat out;
      try {
         out = impl_->run (net, blob);
      } catch (const Ort::Exception& error) {
         if (!net.directml) throw;
         core::Logger::warn (
            "paddle_rec: DirectML inference failed: {}; using CPU", error.what ());
         net = impl_->load (use_title
            ? impl_->model_path.parent_path () / model_files::rec_tooltip_title
            : impl_->model_path, false);
         out = impl_->run (net, blob);
      }

      // Output is [1, timesteps, classes].
      if (out.empty () || out.dims != 3 || out.size [0] != 1) {
         return core::fail (core::Error::make (core::ErrorKind::Ocr,
            "paddle_rec: unexpected output rank {} from model", out.dims));
      }

      auto [text, conf] = ctc_decode (
         out.ptr<float> (), out.size [1], out.size [2],
         use_title ? impl_->title_dict : impl_->dict);

      return RecognizerResult { .text = std::move (text), .confidence = conf };
   } catch (const cv::Exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: inference failed: {}", e.what ()));
   } catch (const Ort::Exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: inference failed: {}", e.what ()));
   }
}

} // namespace gv::ocr

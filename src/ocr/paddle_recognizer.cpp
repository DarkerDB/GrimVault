#include <gv/ocr/paddle_recognizer.h>
#include <gv/core/logger.h>

#include <onnxruntime_cxx_api.h>
#include <opencv2/imgproc.hpp>

#include <fstream>
#include <mutex>

namespace gv::ocr {

namespace {

   // PaddleOCR v5 mobile_rec uses 48x320 CRNN input by default. The recognizer
   // is letterboxed to height 48 keeping aspect ratio, then padded to width 320.
   constexpr int k_model_height = 48;
   constexpr int k_model_width  = 320;

} // namespace

struct PaddleRecognizer::Impl
{
   std::unique_ptr<Ort::Env>            env;
   std::unique_ptr<Ort::SessionOptions> opts;
   std::unique_ptr<Ort::Session>        session;
   std::vector<std::string>             dict;
   LanguageFamily                       family = LanguageFamily::Latin;
   std::mutex                           lock;
};

PaddleRecognizer::PaddleRecognizer ()  : impl_ (std::make_unique<Impl> ()) {}
PaddleRecognizer::~PaddleRecognizer () = default;

LanguageFamily PaddleRecognizer::family () const noexcept                  { return impl_->family; }
void           PaddleRecognizer::set_family (LanguageFamily f) noexcept    { impl_->family = f; }

core::Result<void> PaddleRecognizer::initialize (
   const std::filesystem::path& model_path,
   const std::filesystem::path& dict_path
) {
   try {
      impl_->env  = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_WARNING, "PaddleRec");
      impl_->opts = std::make_unique<Ort::SessionOptions> ();
      impl_->opts->SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);

      // Provider: CPU only for dev builds. vcpkg's stock onnxruntime ships
      // without DirectML or CUDA features. To re-enable GPU later, add
      // those features in vcpkg.json and reinstate the matching
      // OrtSessionOptionsAppendExecutionProvider_* call here.
      core::Logger::info ("paddle_rec: using CPU provider");

      std::wstring wpath { model_path.wstring () };
      impl_->session = std::make_unique<Ort::Session> (*impl_->env, wpath.c_str (), *impl_->opts);

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

      return {};
   } catch (const Ort::Exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: init failed: {}", e.what ()));
   }
}

namespace {

   cv::Mat preprocess (const cv::Mat& input)
   {
      cv::Mat bgr;
      if      (input.channels () == 4) cv::cvtColor (input, bgr, cv::COLOR_BGRA2BGR);
      else if (input.channels () == 3) bgr = input;
      else                              cv::cvtColor (input, bgr, cv::COLOR_GRAY2BGR);

      cv::Mat resized;
      cv::resize (bgr, resized, cv::Size (k_model_width, k_model_height), 0, 0, cv::INTER_CUBIC);

      cv::Mat rgb;
      cv::cvtColor (resized, rgb, cv::COLOR_BGR2RGB);
      rgb.convertTo (rgb, CV_32FC3, 1.0 / 255.0);

      // HWC -> CHW
      std::vector<cv::Mat> channels (3);
      cv::split (rgb, channels);

      cv::Mat chw (1, 3 * k_model_height * k_model_width, CV_32FC1);
      float*  data = chw.ptr<float> ();
      for (int c = 0; c < 3; ++c) {
         std::memcpy (data + c * k_model_height * k_model_width,
                      channels [c].data,
                      k_model_height * k_model_width * sizeof (float));
      }

      return chw;
   }

   // Greedy CTC decode: argmax per timestep, drop blank + consecutive dupes.
   std::pair<std::string, float> ctc_decode (
      const float*                    out,
      const std::vector<int64_t>&     shape,
      const std::vector<std::string>& dict
   ) {
      const int timesteps = static_cast<int> (shape [1]);
      const int classes   = static_cast<int> (shape [2]);
      const int blank     = classes - 1;

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
            if (best_i >= 0 && best_i < static_cast<int> (dict.size ())) {
               result += dict [best_i];
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

core::Result<RecognizerResult> PaddleRecognizer::read (const cv::Mat& line)
{
   if (!impl_->session) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: not initialized"));
   }

   std::lock_guard lock { impl_->lock };

   try {
      cv::Mat chw = preprocess (line);

      std::vector<int64_t> shape  { 1, 3, k_model_height, k_model_width };
      std::vector<float>   buffer { chw.begin<float> (), chw.end<float> () };

      auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value input = Ort::Value::CreateTensor<float> (
         mem, buffer.data (), buffer.size (), shape.data (), shape.size ()
      );

      const char* in_names  [] = { "x" };
      const char* out_names [] = { "fetch_name_0" };

      auto outs = impl_->session->Run (
         Ort::RunOptions { nullptr },
         in_names,  &input, 1,
         out_names, 1
      );

      auto*       out_data  = outs [0].GetTensorMutableData<float> ();
      const auto  out_shape = outs [0].GetTensorTypeAndShapeInfo ().GetShape ();

      auto [text, conf] = ctc_decode (out_data, out_shape, impl_->dict);

      return RecognizerResult { .text = std::move (text), .confidence = conf };
   } catch (const Ort::Exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "paddle_rec: inference failed: {}", e.what ()));
   }
}

} // namespace gv::ocr

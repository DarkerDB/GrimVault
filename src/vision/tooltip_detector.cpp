#include <gv/core/logger.h>
#include <gv/vision/tooltip_detector.h>

#include <dml_provider_factory.h>
#include <onnxruntime_cxx_api.h>

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <mutex>
#include <string>

namespace gv::vision {

namespace {

   constexpr int k_model_size = 416;
   constexpr float k_nms_iou = 0.45f;

} // namespace

struct TooltipDetector::Impl
{
   Ort::Env env { ORT_LOGGING_LEVEL_WARNING, "grimvault" };
   std::unique_ptr<Ort::Session> session;
   std::filesystem::path model_path;
   std::string input_name;
   std::string output_name;
   std::mutex lock;
   float threshold = 0.45f;
   bool directml = false;

   void load (bool gpu)
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

      auto next = std::make_unique<Ort::Session> (env, model_path.c_str (), options);
      Ort::AllocatorWithDefaultOptions allocator;
      input_name = next->GetInputNameAllocated (0, allocator).get ();
      output_name = next->GetOutputNameAllocated (0, allocator).get ();
      session = std::move (next);
      directml = gpu;
   }
};

TooltipDetector::TooltipDetector ()
    : impl_ (std::make_unique<Impl> ())
{
}
TooltipDetector::~TooltipDetector () = default;

core::Result<void> TooltipDetector::initialize (const std::filesystem::path& onnx_path)
{
   if (!std::filesystem::exists (onnx_path)) {
      return core::fail (core::Error::make (
         core::ErrorKind::Ocr, "tooltip_detector: model not found {}", onnx_path.string ()));
   }

   impl_->model_path = onnx_path;
   try {
      impl_->load (true);
      core::Logger::info ("tooltip_detector: DirectML backend");
      return { };
   } catch (const Ort::Exception& error) {
      core::Logger::warn ("tooltip_detector: DirectML unavailable: {}; using CPU", error.what ());
   }

   try {
      impl_->load (false);
      core::Logger::info ("tooltip_detector: CPU fallback backend");
      return { };
   } catch (const Ort::Exception& error) {
      return core::fail (
         core::Error::make (core::ErrorKind::Ocr, "tooltip_detector: {}", error.what ()));
   }
}

void TooltipDetector::set_threshold (float value) noexcept
{
   impl_->threshold = value;
}
float TooltipDetector::threshold () const noexcept
{
   return impl_->threshold;
}

core::Result<std::vector<TooltipBox>> TooltipDetector::detect (const capture::Frame& frame)
{
   if (!impl_->session) {
      return core::fail (
         core::Error::make (core::ErrorKind::Ocr, "tooltip_detector: not initialized"));
   }
   if (frame.empty ()) return std::vector<TooltipBox> { };

   std::lock_guard lock { impl_->lock };

   try {
      cv::Mat bgra (frame.height, frame.width, CV_8UC4, frame.data.get (), frame.stride);
      cv::Mat bgr;
      cv::cvtColor (bgra, bgr, cv::COLOR_BGRA2BGR);

      const float scale = std::min (static_cast<float> (k_model_size) / bgr.cols,
         static_cast<float> (k_model_size) / bgr.rows);
      cv::Mat resized;
      cv::resize (bgr, resized,
         cv::Size { static_cast<int> (bgr.cols * scale), static_cast<int> (bgr.rows * scale) });
      cv::Mat padded (k_model_size, k_model_size, CV_8UC3, cv::Scalar { 114, 114, 114 });
      resized.copyTo (padded (cv::Rect { 0, 0, resized.cols, resized.rows }));

      std::vector<float> input (3 * k_model_size * k_model_size);
      const int plane = k_model_size * k_model_size;
      for (int y = 0; y < k_model_size; ++y) {
         const auto* pixels = padded.ptr<cv::Vec3b> (y);
         for (int x = 0; x < k_model_size; ++x) {
            const int index = y * k_model_size + x;
            input[index] = pixels[x][0];
            input[plane + index] = pixels[x][1];
            input[2 * plane + index] = pixels[x][2];
         }
      }

      const std::array<std::int64_t, 4> shape { 1, 3, k_model_size, k_model_size };
      auto memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
      auto tensor = Ort::Value::CreateTensor<float> (
         memory, input.data (), input.size (), shape.data (), shape.size ());
      const auto run = [&] {
         const std::array<const char*, 1> input_names { impl_->input_name.c_str () };
         const std::array<const char*, 1> output_names { impl_->output_name.c_str () };
         return impl_->session->Run (
            Ort::RunOptions { nullptr }, input_names.data (), &tensor, 1, output_names.data (), 1);
      };
      std::vector<Ort::Value> outputs;
      try {
         outputs = run ();
      } catch (const Ort::Exception& error) {
         if (!impl_->directml) throw;
         core::Logger::warn (
            "tooltip_detector: DirectML inference failed: {}; using CPU", error.what ());
         impl_->load (false);
         outputs = run ();
      }
      const auto output_shape = outputs[0].GetTensorTypeAndShapeInfo ().GetShape ();
      if (output_shape.size () != 3 || output_shape[2] < 6) {
         return core::fail (
            core::Error::make (core::ErrorKind::Ocr, "tooltip_detector: unexpected output shape"));
      }

      const auto rows = static_cast<int> (output_shape[1]);
      const auto dimensions = static_cast<int> (output_shape[2]);
      const float* values = outputs[0].GetTensorData<float> ();
      std::vector<cv::Rect> boxes;
      std::vector<float> scores;
      std::vector<int> class_ids;

      for (int row_index = 0; row_index < rows; ++row_index) {
         const float* row = values + row_index * dimensions;
         const auto class_begin = row + 5;
         const auto class_end = row + dimensions;
         const auto class_at = std::max_element (class_begin, class_end);
         const float score = row[4] * *class_at;
         if (score < impl_->threshold) continue;

         const float left = (row[0] - row[2] * 0.5f) / scale;
         const float top = (row[1] - row[3] * 0.5f) / scale;
         cv::Rect box {
            static_cast<int> (left),
            static_cast<int> (top),
            static_cast<int> (row[2] / scale),
            static_cast<int> (row[3] / scale),
         };
         box &= cv::Rect { 0, 0, frame.width, frame.height };
         if (box.area () <= 0) continue;
         boxes.push_back (box);
         scores.push_back (score);
         class_ids.push_back (static_cast<int> (class_at - class_begin));
      }

      std::vector<int> kept;
      cv::dnn::NMSBoxes (boxes, scores, impl_->threshold, k_nms_iou, kept);
      std::vector<TooltipBox> result;
      result.reserve (kept.size ());
      for (const int index : kept) {
         const auto& box = boxes[index];
         result.push_back ({
            .rect = { box.x, box.y, box.width, box.height },
            .confidence = scores[index],
            .class_id = class_ids[index],
         });
      }
      return result;
   } catch (const Ort::Exception& error) {
      return core::fail (core::Error::make (
         core::ErrorKind::Ocr, "tooltip_detector: inference failed: {}", error.what ()));
   } catch (const cv::Exception& error) {
      return core::fail (core::Error::make (
         core::ErrorKind::Ocr, "tooltip_detector: preprocessing failed: {}", error.what ()));
   }
}

} // namespace gv::vision

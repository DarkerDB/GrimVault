#include <gv/vision/tooltip_detector.h>
#include <gv/core/logger.h>

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <mutex>

namespace gv::vision {

namespace {

   constexpr int   k_model_size = 640;     // 640x640 YOLOv8 input
   constexpr float k_nms_iou    = 0.45f;

} // namespace

struct TooltipDetector::Impl
{
   std::unique_ptr<cv::dnn::Net> net;
   std::mutex                    lock;
   float                         threshold = 0.45f;
};

TooltipDetector::TooltipDetector  () : impl_ (std::make_unique<Impl> ()) {}
TooltipDetector::~TooltipDetector ()                                     = default;

core::Result<void> TooltipDetector::initialize (const std::filesystem::path& onnx_path)
{
   try {
      auto net = std::make_unique<cv::dnn::Net> (cv::dnn::readNetFromONNX (onnx_path.string ()));

      if (net->empty ()) {
         return core::fail (core::Error::make (core::ErrorKind::Ocr,
            "tooltip_detector: failed to load model {}", onnx_path.string ()));
      }

      // CPU backend only. vcpkg's opencv4 port ships without the cuda
      // feature in our manifest, so cv::cuda::* symbols aren't available.
      // Add `opencv4[cuda]` to vcpkg.json + check cv::cuda::getCudaEnabledDeviceCount
      // here to re-enable GPU acceleration when CUDA is available.
      net->setPreferableBackend (cv::dnn::DNN_BACKEND_OPENCV);
      net->setPreferableTarget  (cv::dnn::DNN_TARGET_CPU);
      core::Logger::info ("tooltip_detector: CPU backend");

      impl_->net = std::move (net);
      return {};
   } catch (const cv::Exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "tooltip_detector: cv::Exception: {}", e.what ()));
   }
}

void  TooltipDetector::set_threshold (float t) noexcept { impl_->threshold = t; }
float TooltipDetector::threshold     () const  noexcept { return impl_->threshold; }

core::Result<std::vector<TooltipBox>> TooltipDetector::detect (const capture::Frame& frame)
{
   if (!impl_->net) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr, "tooltip_detector: not initialized"));
   }
   if (frame.empty ()) {
      return std::vector<TooltipBox> {};
   }

   std::lock_guard lock { impl_->lock };

   try {
      cv::Mat bgra (frame.height, frame.width, CV_8UC4, frame.data.get (), frame.stride);
      cv::Mat bgr;
      cv::cvtColor (bgra, bgr, cv::COLOR_BGRA2BGR);

      // Letterbox to square so aspect is preserved.
      const int side = std::max (bgr.cols, bgr.rows);
      cv::Mat   square = cv::Mat::zeros (side, side, CV_8UC3);
      bgr.copyTo (square (cv::Rect (0, 0, bgr.cols, bgr.rows)));

      cv::Mat blob;
      cv::dnn::blobFromImage (
         square,
         blob,
         1.0 / 255.0,
         cv::Size (k_model_size, k_model_size),
         cv::Scalar (),
         /*swapRB=*/ true,
         /*crop=*/   false
      );

      impl_->net->setInput (blob);

      std::vector<cv::Mat> outputs;
      impl_->net->forward (outputs, impl_->net->getUnconnectedOutLayersNames ());

      // YOLOv8 output: [1, 4+num_classes, 8400] — we transpose to [8400, 4+num_classes].
      const int rows = outputs [0].size [2];
      const int dim  = outputs [0].size [1];

      outputs [0] = outputs [0].reshape (1, dim);
      cv::transpose (outputs [0], outputs [0]);

      const float* data    = reinterpret_cast<const float*> (outputs [0].data);
      const float  x_scale = static_cast<float> (side) / k_model_size;
      const float  y_scale = static_cast<float> (side) / k_model_size;

      std::vector<cv::Rect> boxes;
      std::vector<float>    scores;
      std::vector<int>      class_ids;

      const int num_classes = dim - 4;

      for (int i = 0; i < rows; ++i) {
         const float* row     = data + i * dim;
         const float* class_p = row + 4;

         cv::Mat scores_mat (1, num_classes, CV_32FC1, const_cast<float*> (class_p));
         cv::Point max_id;
         double    max_score = 0;
         cv::minMaxLoc (scores_mat, nullptr, &max_score, nullptr, &max_id);

         if (max_score < impl_->threshold) continue;

         const float cx = row [0], cy = row [1], w = row [2], h = row [3];
         cv::Rect rect (
            static_cast<int> ((cx - 0.5f * w) * x_scale),
            static_cast<int> ((cy - 0.5f * h) * y_scale),
            static_cast<int> (w * x_scale),
            static_cast<int> (h * y_scale)
         );

         boxes.push_back (rect);
         scores.push_back (static_cast<float> (max_score));
         class_ids.push_back (max_id.x);
      }

      std::vector<int> kept;
      cv::dnn::NMSBoxes (boxes, scores, impl_->threshold, k_nms_iou, kept);

      std::vector<TooltipBox> out;
      out.reserve (kept.size ());
      for (int idx : kept) {
         out.push_back (TooltipBox {
            .rect       = { boxes [idx].x, boxes [idx].y, boxes [idx].width, boxes [idx].height },
            .confidence = scores [idx],
            .class_id   = class_ids [idx],
         });
      }

      return out;
   } catch (const cv::Exception& e) {
      return core::fail (core::Error::make (core::ErrorKind::Ocr,
         "tooltip_detector: cv::Exception during inference: {}", e.what ()));
   }
}

} // namespace gv::vision

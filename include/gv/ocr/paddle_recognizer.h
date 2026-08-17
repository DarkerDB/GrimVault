#pragma once

#include <gv/core/result.h>
#include <gv/ocr/language.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cv { class Mat; }

namespace gv::ocr {

struct RecognizerResult {
   std::string text;
   float       confidence = 0.0f;
};

// CRNN + CTC recognizer for one language family. Backed by an ONNX session.
// Thread-safe: an internal mutex serializes Read calls. For higher
// throughput, create one Recognizer per worker thread.
class PaddleRecognizer
{
public:
   PaddleRecognizer  ();
   ~PaddleRecognizer ();

   PaddleRecognizer (const PaddleRecognizer&)            = delete;
   PaddleRecognizer& operator= (const PaddleRecognizer&) = delete;

   // model_path:  path to the chosen <family>/rec-*.onnx
   // dict_path:   newline-separated character dictionary (CTC blank at last index)
   core::Result<void> initialize (
      const std::filesystem::path& model_path,
      const std::filesystem::path& dict_path
   );

   // Read a single text-line image (BGRA or BGR cv::Mat). Returns decoded
   // text and a confidence score (mean per-step argmax probability).
   core::Result<RecognizerResult> read (const cv::Mat& line, bool title = false);

   // English may provide an independently trained title-face model beside
   // the body recognizer. Other languages transparently use the body model.
   bool has_title_model () const noexcept;

   LanguageFamily family () const noexcept;
   void           set_family (LanguageFamily f) noexcept;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::ocr

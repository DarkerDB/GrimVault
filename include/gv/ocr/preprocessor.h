#pragma once

#include <opencv2/core.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace gv::ocr::preprocess {

// Pure geometry/preprocessing primitives for the single-line recognizer.
std::vector<cv::Range> line_bands (const cv::Mat& crop);
std::size_t            first_tooltip_band (const cv::Mat& crop, const std::vector<cv::Range>& bands);
std::optional<std::size_t> title_band (const cv::Mat& crop, const std::vector<cv::Range>& bands);
bool                   top_is_clipped (const cv::Mat& crop, const std::vector<cv::Range>& bands);
cv::Mat                trim_cols  (const cv::Mat& line);
std::vector<cv::Range> col_chunks (const cv::Mat& line);

// Remove a full-width separator accidentally merged into the centered title
// band after dark-label thresholding.
cv::Mat trim_title_rule (const cv::Mat& line);

// True for thin full-width ornaments that contain no useful text.
bool is_horizontal_rule (const cv::Mat& line);
bool is_item_tooltip    (const cv::Mat& crop, std::size_t line_count);

} // namespace gv::ocr::preprocess

#pragma once

#include <opencv2/core/mat.hpp>

#include <optional>
#include <string>

namespace gv::vision {

// Detect the socket glyph in the side gutters of one full-width tooltip stat
// line. Conservative by design: no observation is safer than a wrong family.
std::optional<std::string> detect_gem_family (const cv::Mat& bgra_line);

} // namespace gv::vision

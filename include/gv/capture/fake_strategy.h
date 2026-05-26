#pragma once

#include <gv/capture/i_capture_strategy.h>

#include <filesystem>
#include <memory>

namespace gv::capture {

// Test/demo strategy that replays PNG fixtures. capture_window / capture_monitor
// return frames in the order they were loaded, looping at the end.
//
// Loaded via load_directory(<dir>) — reads <dir>/*.png. PNG decoding is
// internal (stb_image) to avoid coupling capture/ to OpenCV.
class FakeStrategy : public ICaptureStrategy
{
public:
   FakeStrategy ();
   ~FakeStrategy () override;

   // Append a single fixture from a path.
   core::Result<void> load_image (const std::filesystem::path& path);

   // Append every *.png under a directory (non-recursive). Order is alphabetical.
   core::Result<void> load_directory (const std::filesystem::path& dir);

   // Synthetic frame for unit tests that don't care about real pixels.
   void push_solid (int width, int height, std::uint8_t r, std::uint8_t g, std::uint8_t b);

   core::Result<void>  initialize ()                      override;
   void                shutdown   ()         noexcept      override;
   core::Result<Frame> capture_window  (void* window)      override;
   core::Result<Frame> capture_monitor (void* monitor)     override;

   std::string_view name   () const noexcept override;
   std::string_view reason () const noexcept override;

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::capture

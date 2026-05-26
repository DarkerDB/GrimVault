#include <gv/capture/fake_strategy.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace gv::capture {

struct FakeStrategy::Impl
{
   std::vector<Frame> frames;
   std::size_t        cursor = 0;
};

FakeStrategy::FakeStrategy  () : impl_ (std::make_unique<Impl> ()) {}
FakeStrategy::~FakeStrategy ()                                     = default;

core::Result<void> FakeStrategy::load_image (const std::filesystem::path& path)
{
   int w = 0, h = 0, channels = 0;
   stbi_uc* raw = stbi_load (path.string ().c_str (), &w, &h, &channels, 4);

   if (!raw) {
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "fake: failed to load {}", path.string ()));
   }

   const int stride = w * 4;
   auto pixels = std::shared_ptr<std::uint8_t []> (
      new std::uint8_t [ static_cast<std::size_t> (stride) * h ],
      std::default_delete<std::uint8_t []> ()
   );

   // stb_image returns RGBA; we want BGRA. Swap R↔B per pixel.
   for (int i = 0; i < w * h; ++i) {
      pixels [4 * i + 0] = raw [4 * i + 2];
      pixels [4 * i + 1] = raw [4 * i + 1];
      pixels [4 * i + 2] = raw [4 * i + 0];
      pixels [4 * i + 3] = raw [4 * i + 3];
   }

   stbi_image_free (raw);

   impl_->frames.push_back (Frame {
      .data       = std::move (pixels),
      .width      = w,
      .height     = h,
      .stride     = stride,
      .dpi_scale  = 1.0,
      .monitor_id = 0,
      .window_id  = 0,
      .timestamp  = std::chrono::steady_clock::now (),
   });

   return {};
}

core::Result<void> FakeStrategy::load_directory (const std::filesystem::path& dir)
{
   std::error_code ec;
   if (!std::filesystem::is_directory (dir, ec)) {
      return core::fail (core::Error::make (core::ErrorKind::NotFound,
         "fake: directory {} not found", dir.string ()));
   }

   std::vector<std::filesystem::path> pngs;

   for (auto& entry : std::filesystem::directory_iterator (dir)) {
      if (entry.path ().extension () == ".png") {
         pngs.push_back (entry.path ());
      }
   }

   std::sort (pngs.begin (), pngs.end ());

   for (const auto& p : pngs) {
      auto r = load_image (p);
      if (!r.has_value ()) return r;
   }

   return {};
}

void FakeStrategy::push_solid (int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
   const int stride = w * 4;
   auto pixels = std::shared_ptr<std::uint8_t []> (
      new std::uint8_t [ static_cast<std::size_t> (stride) * h ],
      std::default_delete<std::uint8_t []> ()
   );

   for (int i = 0; i < w * h; ++i) {
      pixels [4 * i + 0] = b;
      pixels [4 * i + 1] = g;
      pixels [4 * i + 2] = r;
      pixels [4 * i + 3] = 0xff;
   }

   impl_->frames.push_back (Frame {
      .data       = std::move (pixels),
      .width      = w,
      .height     = h,
      .stride     = stride,
      .dpi_scale  = 1.0,
      .monitor_id = 0,
      .window_id  = 0,
      .timestamp  = std::chrono::steady_clock::now (),
   });
}

core::Result<void> FakeStrategy::initialize ()              { return {}; }
void               FakeStrategy::shutdown   () noexcept     { if (impl_) impl_->frames.clear (); }

core::Result<Frame> FakeStrategy::capture_window  (void*) { return capture_monitor (nullptr); }

core::Result<Frame> FakeStrategy::capture_monitor (void*)
{
   if (!impl_ || impl_->frames.empty ()) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "fake: no frames loaded"));
   }

   const auto& f = impl_->frames [impl_->cursor];
   impl_->cursor = (impl_->cursor + 1) % impl_->frames.size ();

   Frame copy        = f;
   copy.timestamp    = std::chrono::steady_clock::now ();
   return copy;
}

std::string_view FakeStrategy::name   () const noexcept { return "fake"; }
std::string_view FakeStrategy::reason () const noexcept { return "Replays loaded PNG fixtures (for tests and demos)"; }

} // namespace gv::capture

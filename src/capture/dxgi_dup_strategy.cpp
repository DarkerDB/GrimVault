#include "dxgi_dup_strategy.h"

#include <gv/core/logger.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace gv::capture {

using Microsoft::WRL::ComPtr;

namespace {

HMONITOR resolve_monitor (HMONITOR monitor)
{
   if (monitor) return monitor;
   POINT cursor {};
   if (!::GetCursorPos (&cursor)) cursor = { 0, 0 };
   return ::MonitorFromPoint (cursor, MONITOR_DEFAULTTOPRIMARY);
}

core::Result<Frame> crop_to_window (Frame source, HWND window)
{
   RECT client {};
   POINT origin {};
   if (!::GetClientRect (window, &client) || !::ClientToScreen (window, &origin)) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture, "dxgi: could not resolve window client bounds"));
   }

   RECT wanted {
      origin.x,
      origin.y,
      origin.x + client.right - client.left,
      origin.y + client.bottom - client.top,
   };
   RECT available {
      source.origin_x,
      source.origin_y,
      source.origin_x + source.width,
      source.origin_y + source.height,
   };
   RECT clipped {
      std::max (wanted.left, available.left),
      std::max (wanted.top, available.top),
      std::min (wanted.right, available.right),
      std::min (wanted.bottom, available.bottom),
   };
   if (clipped.right <= clipped.left || clipped.bottom <= clipped.top) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture, "dxgi: game window is outside the duplicated monitor"));
   }

   const int width  = clipped.right - clipped.left;
   const int height = clipped.bottom - clipped.top;
   const int stride = width * 4;
   auto pixels = std::shared_ptr<std::uint8_t []> (
      new std::uint8_t [static_cast<std::size_t> (stride) * height]);

   const int source_x = clipped.left - source.origin_x;
   const int source_y = clipped.top  - source.origin_y;
   for (int row = 0; row < height; ++row) {
      const auto* from = source.data.get ()
         + static_cast<std::size_t> (source_y + row) * source.stride
         + static_cast<std::size_t> (source_x) * 4;
      auto* to = pixels.get () + static_cast<std::size_t> (row) * stride;
      std::memcpy (to, from, static_cast<std::size_t> (stride));
   }

   source.data      = std::move (pixels);
   source.width     = width;
   source.height    = height;
   source.stride    = stride;
   source.origin_x  = clipped.left;
   source.origin_y  = clipped.top;
   source.window_id = reinterpret_cast<std::uintptr_t> (window);
   return source;
}

} // namespace

struct DxgiDuplicationStrategy::Impl
{
   ComPtr<ID3D11Device>             device;
   ComPtr<ID3D11DeviceContext>      context;
   ComPtr<IDXGIOutputDuplication>   duplication;
   ComPtr<IDXGIOutput1>             output;
   DXGI_OUTPUT_DESC                 output_desc {};
   Frame                            last_frame;
   HMONITOR                         monitor = nullptr;
   bool                             initialized = false;

   void reset ()
   {
      duplication.Reset ();
      output.Reset ();
      context.Reset ();
      device.Reset ();
      output_desc = {};
      last_frame = {};
      monitor = nullptr;
      initialized = false;
   }

   core::Result<void> select (HMONITOR wanted)
   {
      wanted = resolve_monitor (wanted);
      if (duplication && monitor == wanted) return {};
      reset ();

      ComPtr<IDXGIFactory1> factory;
      HRESULT hr = ::CreateDXGIFactory1 (IID_PPV_ARGS (factory.GetAddressOf ()));
      if (FAILED (hr)) {
         return core::fail (core::Error::make (
            core::ErrorKind::Capture, "dxgi: CreateDXGIFactory1 failed hr=0x{:08x}",
            static_cast<unsigned> (hr)));
      }

      for (UINT adapter_index = 0;; ++adapter_index) {
         ComPtr<IDXGIAdapter1> adapter;
         if (factory->EnumAdapters1 (adapter_index, adapter.GetAddressOf ()) == DXGI_ERROR_NOT_FOUND) {
            break;
         }

         for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> candidate;
            if (adapter->EnumOutputs (output_index, candidate.GetAddressOf ()) == DXGI_ERROR_NOT_FOUND) {
               break;
            }

            DXGI_OUTPUT_DESC desc {};
            if (FAILED (candidate->GetDesc (&desc)) || desc.Monitor != wanted) continue;

            constexpr D3D_FEATURE_LEVEL levels [] = {
               D3D_FEATURE_LEVEL_11_1,
               D3D_FEATURE_LEVEL_11_0,
               D3D_FEATURE_LEVEL_10_1,
               D3D_FEATURE_LEVEL_10_0,
            };
            D3D_FEATURE_LEVEL selected {};
            hr = ::D3D11CreateDevice (
               adapter.Get (), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
               D3D11_CREATE_DEVICE_BGRA_SUPPORT,
               levels, ARRAYSIZE (levels), D3D11_SDK_VERSION,
               device.GetAddressOf (), &selected, context.GetAddressOf ());
            if (FAILED (hr)) continue;

            ComPtr<IDXGIOutput1> output1;
            hr = candidate.As (&output1);
            if (SUCCEEDED (hr)) {
               hr = output1->DuplicateOutput (device.Get (), duplication.GetAddressOf ());
            }
            if (FAILED (hr)) {
               reset ();
               continue;
            }

            output      = output1;
            output_desc = desc;
            monitor     = wanted;
            initialized = true;
            core::Logger::info ("dxgi: selected monitor at {},{} ({}x{})",
               desc.DesktopCoordinates.left,
               desc.DesktopCoordinates.top,
               desc.DesktopCoordinates.right - desc.DesktopCoordinates.left,
               desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
            return {};
         }
      }

      return core::fail (core::Error::make (
         core::ErrorKind::Capture, "dxgi: no duplicable output matched the requested monitor"));
   }

   core::Result<Frame> read (bool& access_lost)
   {
      access_lost = false;
      DXGI_OUTDUPL_FRAME_INFO info {};
      ComPtr<IDXGIResource> resource;
      HRESULT hr = duplication->AcquireNextFrame (200, &info, resource.GetAddressOf ());
      if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
         if (!last_frame.empty ()) {
            auto snapshot = last_frame;
            snapshot.timestamp = std::chrono::steady_clock::now ();
            snapshot.cursor = cursor_now ();
            return snapshot;
         }
         return core::fail (core::Error::make (
            core::ErrorKind::Capture, "dxgi: AcquireNextFrame timeout"));
      }
      if (hr == DXGI_ERROR_ACCESS_LOST) access_lost = true;
      if (FAILED (hr)) {
         return core::fail (core::Error::make (
            core::ErrorKind::Capture, "dxgi: AcquireNextFrame failed hr=0x{:08x}",
            static_cast<unsigned> (hr)));
      }

      struct Release {
         IDXGIOutputDuplication* duplication;
         ~Release () { duplication->ReleaseFrame (); }
      } release { duplication.Get () };

      ComPtr<ID3D11Texture2D> source;
      hr = resource.As (&source);
      if (FAILED (hr)) {
         return core::fail (core::Error::make (
            core::ErrorKind::Capture, "dxgi: query texture failed"));
      }

      D3D11_TEXTURE2D_DESC desc {};
      source->GetDesc (&desc);
      D3D11_TEXTURE2D_DESC staging = desc;
      staging.Usage          = D3D11_USAGE_STAGING;
      staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      staging.BindFlags      = 0;
      staging.MiscFlags      = 0;

      ComPtr<ID3D11Texture2D> staging_texture;
      hr = device->CreateTexture2D (&staging, nullptr, staging_texture.GetAddressOf ());
      if (FAILED (hr)) {
         return core::fail (core::Error::make (
            core::ErrorKind::Capture, "dxgi: CreateTexture2D failed"));
      }

      context->CopyResource (staging_texture.Get (), source.Get ());
      D3D11_MAPPED_SUBRESOURCE map {};
      hr = context->Map (staging_texture.Get (), 0, D3D11_MAP_READ, 0, &map);
      if (FAILED (hr)) {
         return core::fail (core::Error::make (core::ErrorKind::Capture, "dxgi: Map failed"));
      }

      const int width  = static_cast<int> (desc.Width);
      const int height = static_cast<int> (desc.Height);
      const int stride = static_cast<int> (map.RowPitch);
      auto pixels = std::shared_ptr<std::uint8_t []> (
         new std::uint8_t [static_cast<std::size_t> (stride) * height]);
      std::memcpy (pixels.get (), map.pData, static_cast<std::size_t> (stride) * height);
      context->Unmap (staging_texture.Get (), 0);

      Frame frame {
         .data       = std::move (pixels),
         .width      = width,
         .height     = height,
         .stride     = stride,
         .origin_x   = output_desc.DesktopCoordinates.left,
         .origin_y   = output_desc.DesktopCoordinates.top,
         .dpi_scale  = 1.0,
         .monitor_id = reinterpret_cast<std::uintptr_t> (monitor),
         .window_id  = 0,
         .timestamp  = std::chrono::steady_clock::now (),
         .cursor     = cursor_now (),
      };
      last_frame = frame;
      return frame;
   }

   core::Result<Frame> capture (HMONITOR wanted)
   {
      auto selected = select (wanted);
      if (!selected.has_value ()) return core::fail (selected.error ());

      bool access_lost = false;
      auto frame = read (access_lost);
      if (!access_lost) return frame;

      core::Logger::warn ("dxgi: access lost; recreating desktop duplication");
      const auto lost_monitor = monitor;
      reset ();
      selected = select (lost_monitor);
      if (!selected.has_value ()) return core::fail (selected.error ());
      return read (access_lost);
   }
};

DxgiDuplicationStrategy::DxgiDuplicationStrategy ()
   : impl_ (std::make_unique<Impl> ())
{}

DxgiDuplicationStrategy::~DxgiDuplicationStrategy () = default;

core::Result<void> DxgiDuplicationStrategy::initialize ()
{
   if (impl_->initialized) return {};
   return impl_->select (nullptr);
}

void DxgiDuplicationStrategy::shutdown () noexcept
{
   if (!impl_) return;
   impl_->reset ();
   impl_->initialized = false;
}

core::Result<Frame> DxgiDuplicationStrategy::capture_window (void* window)
{
   const auto hwnd = static_cast<HWND> (window);
   if (!::IsWindow (hwnd)) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "dxgi: invalid HWND"));
   }

   const auto monitor = ::MonitorFromWindow (hwnd, MONITOR_DEFAULTTONEAREST);
   auto frame = impl_->capture (monitor);
   if (!frame.has_value ()) return frame;
   return crop_to_window (std::move (*frame), hwnd);
}

core::Result<Frame> DxgiDuplicationStrategy::capture_monitor (void* monitor)
{
   return impl_->capture (resolve_monitor (static_cast<HMONITOR> (monitor)));
}

std::string_view DxgiDuplicationStrategy::name () const noexcept
{
   return "dxgi.duplication";
}

std::string_view DxgiDuplicationStrategy::reason () const noexcept
{
   return "Windows 10+; monitor-aware Desktop Duplication fallback with window cropping";
}

} // namespace gv::capture

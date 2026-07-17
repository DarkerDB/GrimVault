#include "dxgi_dup_strategy.h"

#include <gv/core/logger.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>

namespace gv::capture {

using Microsoft::WRL::ComPtr;

struct DxgiDuplicationStrategy::Impl
{
   ComPtr<ID3D11Device>             device;
   ComPtr<ID3D11DeviceContext>      context;
   ComPtr<IDXGIOutputDuplication>   duplication;
   ComPtr<IDXGIOutput1>             output;
   DXGI_OUTPUT_DESC                 output_desc {};
   std::uint64_t                    monitor_id = 0;
   bool                             initialized = false;
};

DxgiDuplicationStrategy::DxgiDuplicationStrategy  () : impl_ (std::make_unique<Impl> ()) {}
DxgiDuplicationStrategy::~DxgiDuplicationStrategy ()                                     = default;

core::Result<void> DxgiDuplicationStrategy::initialize ()
{
   if (impl_->initialized) return {};

   D3D_FEATURE_LEVEL fl;
   HRESULT hr = D3D11CreateDevice (
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      nullptr, 0,
      D3D11_SDK_VERSION,
      impl_->device.GetAddressOf (),
      &fl,
      impl_->context.GetAddressOf ()
   );

   if (FAILED (hr)) {
      return core::fail (core::Error::make (core::ErrorKind::Capture,
         "dxgi: D3D11CreateDevice failed hr=0x{:08x}", static_cast<unsigned> (hr)));
   }

   ComPtr<IDXGIDevice>  dxgi_device;
   ComPtr<IDXGIAdapter> adapter;
   hr = impl_->device.As (&dxgi_device);
   if (SUCCEEDED (hr)) hr = dxgi_device->GetAdapter (adapter.GetAddressOf ());

   if (FAILED (hr)) {
      return core::fail (core::Error::make (core::ErrorKind::Capture,
         "dxgi: get adapter failed hr=0x{:08x}", static_cast<unsigned> (hr)));
   }

   ComPtr<IDXGIOutput>  output0;
   ComPtr<IDXGIOutput1> output1;
   hr = adapter->EnumOutputs (0, output0.GetAddressOf ());
   if (SUCCEEDED (hr)) hr = output0.As (&output1);
   if (SUCCEEDED (hr)) hr = output1->DuplicateOutput (impl_->device.Get (), impl_->duplication.GetAddressOf ());

   if (FAILED (hr)) {
      return core::fail (core::Error::make (core::ErrorKind::Capture,
         "dxgi: DuplicateOutput failed hr=0x{:08x}", static_cast<unsigned> (hr)));
   }

   impl_->output = output1;
   output1->GetDesc (&impl_->output_desc);
   impl_->monitor_id  = reinterpret_cast<std::uintptr_t> (impl_->output_desc.Monitor);
   impl_->initialized = true;

   core::Logger::info ("dxgi: initialized — monitor={}x{}",
      impl_->output_desc.DesktopCoordinates.right  - impl_->output_desc.DesktopCoordinates.left,
      impl_->output_desc.DesktopCoordinates.bottom - impl_->output_desc.DesktopCoordinates.top);

   return {};
}

void DxgiDuplicationStrategy::shutdown () noexcept
{
   if (!impl_) return;
   impl_->duplication.Reset ();
   impl_->output.Reset ();
   impl_->context.Reset ();
   impl_->device.Reset ();
   impl_->initialized = false;
}

namespace {

   core::Result<Frame> read_one_frame (
      const ComPtr<ID3D11Device>&            device,
      const ComPtr<ID3D11DeviceContext>&     context,
      const ComPtr<IDXGIOutputDuplication>&  dup,
      std::uint64_t                          monitor_id
   ) {
      DXGI_OUTDUPL_FRAME_INFO info {};
      ComPtr<IDXGIResource>   resource;

      HRESULT hr = dup->AcquireNextFrame (200, &info, resource.GetAddressOf ());

      if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
         return core::fail (core::Error::make (core::ErrorKind::Capture,
            "dxgi: AcquireNextFrame timeout"));
      }

      if (FAILED (hr)) {
         return core::fail (core::Error::make (core::ErrorKind::Capture,
            "dxgi: AcquireNextFrame failed hr=0x{:08x}", static_cast<unsigned> (hr)));
      }

      ComPtr<ID3D11Texture2D> source;
      hr = resource.As (&source);
      if (FAILED (hr)) { dup->ReleaseFrame (); return core::fail (core::Error::make (core::ErrorKind::Capture, "dxgi: query texture failed")); }

      D3D11_TEXTURE2D_DESC desc {};
      source->GetDesc (&desc);

      D3D11_TEXTURE2D_DESC staging = desc;
      staging.Usage          = D3D11_USAGE_STAGING;
      staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      staging.BindFlags      = 0;
      staging.MiscFlags      = 0;

      ComPtr<ID3D11Texture2D> staging_tex;
      hr = device->CreateTexture2D (&staging, nullptr, staging_tex.GetAddressOf ());
      if (FAILED (hr)) { dup->ReleaseFrame (); return core::fail (core::Error::make (core::ErrorKind::Capture, "dxgi: CreateTexture2D failed")); }

      context->CopyResource (staging_tex.Get (), source.Get ());

      D3D11_MAPPED_SUBRESOURCE map {};
      hr = context->Map (staging_tex.Get (), 0, D3D11_MAP_READ, 0, &map);
      if (FAILED (hr)) { dup->ReleaseFrame (); return core::fail (core::Error::make (core::ErrorKind::Capture, "dxgi: Map failed")); }

      const int width  = static_cast<int> (desc.Width);
      const int height = static_cast<int> (desc.Height);
      const int stride = static_cast<int> (map.RowPitch);

      auto pixels = std::shared_ptr<std::uint8_t []> (
         new std::uint8_t [ static_cast<std::size_t> (stride) * height ]
      );
      std::memcpy (pixels.get (), map.pData, static_cast<std::size_t> (stride) * height);

      context->Unmap (staging_tex.Get (), 0);
      dup->ReleaseFrame ();

      return Frame {
         .data       = std::move (pixels),
         .width      = width,
         .height     = height,
         .stride     = stride,
         .dpi_scale  = 1.0,
         .monitor_id = monitor_id,
         .window_id  = 0,
         .timestamp  = std::chrono::steady_clock::now (),
         .cursor     = cursor_now (),
      };
   }

} // namespace

core::Result<Frame> DxgiDuplicationStrategy::capture_window (void* /*window*/)
{
   if (!impl_->initialized) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "dxgi: not initialized"));
   }

   // DXGI duplicates the entire monitor; window-region cropping is done by
   // the caller (vision stage) based on the game window's screen bounds.
   return read_one_frame (impl_->device, impl_->context, impl_->duplication, impl_->monitor_id);
}

core::Result<Frame> DxgiDuplicationStrategy::capture_monitor (void* /*monitor*/)
{
   if (!impl_->initialized) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "dxgi: not initialized"));
   }

   return read_one_frame (impl_->device, impl_->context, impl_->duplication, impl_->monitor_id);
}

std::string_view DxgiDuplicationStrategy::name   () const noexcept { return "dxgi.duplication"; }
std::string_view DxgiDuplicationStrategy::reason () const noexcept { return "Windows 8+; Desktop Duplication fallback (no per-window capture, vision crops)"; }

} // namespace gv::capture

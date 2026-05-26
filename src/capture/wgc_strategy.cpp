#include "wgc_strategy.h"

#include <gv/core/logger.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <Windows.Graphics.Capture.Interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>

// IDirect3DDxgiInterfaceAccess lets us bridge a WinRT IDirect3DSurface to
// the underlying DXGI/ID3D11Texture2D. Declared manually because the SDK
// header doesn't always expose it.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDirect3DDxgiInterfaceAccess : IUnknown
{
   virtual HRESULT __stdcall GetInterface (REFIID riid, void** ppvObject) = 0;
};

namespace gv::capture {

namespace {

   using Microsoft::WRL::ComPtr;
   using namespace winrt::Windows::Foundation;
   using namespace winrt::Windows::Foundation::Metadata;
   using namespace winrt::Windows::Graphics;
   using namespace winrt::Windows::Graphics::Capture;
   using namespace winrt::Windows::Graphics::DirectX;
   using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

   bool detect_borderless_supported ()
   {
      try {
         return ApiInformation::IsPropertyPresent (
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsBorderRequired"
         );
      } catch (...) {
         return false;
      }
   }

   bool detect_cursor_toggle_supported ()
   {
      try {
         return ApiInformation::IsPropertyPresent (
            L"Windows.Graphics.Capture.GraphicsCaptureSession",
            L"IsCursorCaptureEnabled"
         );
      } catch (...) {
         return false;
      }
   }

} // namespace

struct WgcStrategy::Impl
{
   ComPtr<ID3D11Device>        d3d_device;
   ComPtr<ID3D11DeviceContext> d3d_context;
   IDirect3DDevice             winrt_device { nullptr };

   bool        borderless_supported  = false;
   bool        cursor_toggle_supported = false;
   bool        initialized           = false;
   std::string reason_text;

   // ---- Continuous capture state (mutex-protected) ----
   std::mutex                                cont_lock;
   std::condition_variable                   cont_cv;
   Direct3D11CaptureFramePool                cont_pool    { nullptr };
   GraphicsCaptureSession                    cont_session { nullptr };
   winrt::event_token                        cont_token   {};
   Frame                                     cont_latest;
   std::atomic<bool>                         cont_active  { false };
   std::uint64_t                             cont_monitor_id = 0;
   std::uint64_t                             cont_window_id  = 0;

   bool init_d3d ()
   {
      D3D_FEATURE_LEVEL levels [] = {
         D3D_FEATURE_LEVEL_11_1,
         D3D_FEATURE_LEVEL_11_0,
         D3D_FEATURE_LEVEL_10_1,
         D3D_FEATURE_LEVEL_10_0,
      };

      D3D_FEATURE_LEVEL picked;

      HRESULT hr = D3D11CreateDevice (
         nullptr,
         D3D_DRIVER_TYPE_HARDWARE,
         nullptr,
         D3D11_CREATE_DEVICE_BGRA_SUPPORT,
         levels, ARRAYSIZE (levels),
         D3D11_SDK_VERSION,
         d3d_device.GetAddressOf (),
         &picked,
         d3d_context.GetAddressOf ()
      );

      if (FAILED (hr)) {
         core::Logger::error ("wgc: D3D11CreateDevice failed hr=0x{:08x}", static_cast<unsigned> (hr));
         return false;
      }

      ComPtr<IDXGIDevice> dxgi;
      hr = d3d_device.As (&dxgi);

      if (FAILED (hr)) {
         core::Logger::error ("wgc: query IDXGIDevice failed hr=0x{:08x}", static_cast<unsigned> (hr));
         return false;
      }

      ::IInspectable* inspectable = nullptr;
      hr = ::CreateDirect3D11DeviceFromDXGIDevice (dxgi.Get (), &inspectable);

      if (FAILED (hr) || !inspectable) {
         core::Logger::error ("wgc: CreateDirect3D11DeviceFromDXGIDevice failed hr=0x{:08x}", static_cast<unsigned> (hr));
         return false;
      }

      winrt_device = { inspectable, winrt::take_ownership_from_abi };
      return true;
   }

   GraphicsCaptureItem item_for_window (HWND hwnd) const
   {
      auto factory = winrt::get_activation_factory<GraphicsCaptureItem> ();
      auto interop = factory.as<::IGraphicsCaptureItemInterop> ();

      GraphicsCaptureItem item { nullptr };
      winrt::check_hresult (interop->CreateForWindow (
         hwnd,
         winrt::guid_of<GraphicsCaptureItem> (),
         winrt::put_abi (item)
      ));

      return item;
   }

   GraphicsCaptureItem item_for_monitor (HMONITOR hmon) const
   {
      auto factory = winrt::get_activation_factory<GraphicsCaptureItem> ();
      auto interop = factory.as<::IGraphicsCaptureItemInterop> ();

      GraphicsCaptureItem item { nullptr };
      winrt::check_hresult (interop->CreateForMonitor (
         hmon,
         winrt::guid_of<GraphicsCaptureItem> (),
         winrt::put_abi (item)
      ));

      return item;
   }

   core::Result<Frame> capture_item (
      const GraphicsCaptureItem& item,
      std::uint64_t              monitor_id,
      std::uint64_t              window_id
   ) {
      const auto size = item.Size ();

      auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded (
         winrt_device,
         DirectXPixelFormat::B8G8R8A8UIntNormalized,
         2,
         size
      );

      auto session = pool.CreateCaptureSession (item);

      if (borderless_supported) {
         try { session.IsBorderRequired (false); }
         catch (const winrt::hresult_error& e) {
            core::Logger::warn ("wgc: IsBorderRequired=false failed: {}",
               winrt::to_string (e.message ()));
         }
      }

      if (cursor_toggle_supported) {
         try { session.IsCursorCaptureEnabled (false); }
         catch (const winrt::hresult_error&) { /* ignore */ }
      }

      std::mutex              m;
      std::condition_variable cv;
      bool                    have_frame = false;
      Direct3D11CaptureFrame  captured { nullptr };

      auto token = pool.FrameArrived ([&] (auto&& sender, auto&&) {
         auto next = sender.TryGetNextFrame ();
         if (!next) return;

         {
            std::lock_guard lock (m);
            captured   = next;
            have_frame = true;
         }
         cv.notify_one ();
      });

      session.StartCapture ();

      {
         std::unique_lock lock (m);
         if (!cv.wait_for (lock, std::chrono::milliseconds (500), [&] { return have_frame; })) {
            pool.FrameArrived (token);
            session.Close ();
            pool.Close ();
            return core::fail (core::Error::make (
               core::ErrorKind::Capture,
               "wgc: timed out waiting for first frame"
            ));
         }
      }

      pool.FrameArrived (token);

      auto surface = captured.Surface ();
      auto access  = surface.as<::IDirect3DDxgiInterfaceAccess> ();

      ComPtr<ID3D11Texture2D> source;
      winrt::check_hresult (access->GetInterface (
         __uuidof (ID3D11Texture2D),
         reinterpret_cast<void**> (source.GetAddressOf ())
      ));

      D3D11_TEXTURE2D_DESC desc {};
      source->GetDesc (&desc);

      D3D11_TEXTURE2D_DESC staging_desc = desc;
      staging_desc.Usage          = D3D11_USAGE_STAGING;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      staging_desc.BindFlags      = 0;
      staging_desc.MiscFlags      = 0;

      ComPtr<ID3D11Texture2D> staging;
      HRESULT hr = d3d_device->CreateTexture2D (&staging_desc, nullptr, staging.GetAddressOf ());

      if (FAILED (hr)) {
         session.Close ();
         pool.Close ();
         return core::fail (core::Error::make (
            core::ErrorKind::Capture,
            "wgc: CreateTexture2D(staging) failed hr=0x{:08x}",
            static_cast<unsigned> (hr)
         ));
      }

      d3d_context->CopyResource (staging.Get (), source.Get ());

      D3D11_MAPPED_SUBRESOURCE map {};
      hr = d3d_context->Map (staging.Get (), 0, D3D11_MAP_READ, 0, &map);

      if (FAILED (hr)) {
         session.Close ();
         pool.Close ();
         return core::fail (core::Error::make (
            core::ErrorKind::Capture,
            "wgc: Map(staging) failed hr=0x{:08x}",
            static_cast<unsigned> (hr)
         ));
      }

      const int width  = static_cast<int> (desc.Width);
      const int height = static_cast<int> (desc.Height);
      const int stride = static_cast<int> (map.RowPitch);

      auto pixels = std::shared_ptr<std::uint8_t []> (
         new std::uint8_t [ static_cast<std::size_t> (stride) * height ],
         std::default_delete<std::uint8_t []> ()
      );

      std::memcpy (pixels.get (), map.pData, static_cast<std::size_t> (stride) * height);

      d3d_context->Unmap (staging.Get (), 0);
      session.Close ();
      pool.Close ();

      return Frame {
         .data       = std::move (pixels),
         .width      = width,
         .height     = height,
         .stride     = stride,
         .dpi_scale  = 1.0,                  // caller may set from HMONITOR DPI
         .monitor_id = monitor_id,
         .window_id  = window_id,
         .timestamp  = std::chrono::steady_clock::now (),
      };
   }
};

WgcStrategy::WgcStrategy  () : impl_ (std::make_unique<Impl> ()) {}
WgcStrategy::~WgcStrategy ()                                     = default;

core::Result<void> WgcStrategy::initialize ()
{
   if (impl_->initialized) return {};

   if (!GraphicsCaptureSession::IsSupported ()) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture,
         "wgc: Windows.Graphics.Capture is not supported on this system"
      ));
   }

   HRESULT com = ::CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED);
   if (FAILED (com) && com != RPC_E_CHANGED_MODE) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture,
         "wgc: CoInitializeEx failed hr=0x{:08x}", static_cast<unsigned> (com)
      ));
   }

   if (!impl_->init_d3d ()) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture, "wgc: D3D11 init failed"
      ));
   }

   impl_->borderless_supported    = detect_borderless_supported ();
   impl_->cursor_toggle_supported = detect_cursor_toggle_supported ();
   impl_->initialized             = true;
   impl_->reason_text             = impl_->borderless_supported
      ? "Windows 10 20H1+ / Windows 11; IsBorderRequired supported, capture is borderless"
      : "Windows 10 1803..19045; standard WGC (yellow capture border visible)";

   core::Logger::info ("wgc: initialized — borderless={} cursor_toggle={}",
      impl_->borderless_supported, impl_->cursor_toggle_supported);

   return {};
}

void WgcStrategy::shutdown () noexcept
{
   if (!impl_) return;

   impl_->winrt_device = nullptr;
   impl_->d3d_context.Reset ();
   impl_->d3d_device.Reset ();
   impl_->initialized = false;
}

core::Result<Frame> WgcStrategy::capture_window (void* window)
{
   if (!impl_->initialized) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "wgc: not initialized"));
   }

   auto hwnd = static_cast<HWND> (window);

   try {
      auto item = impl_->item_for_window (hwnd);
      return impl_->capture_item (
         item,
         0,
         reinterpret_cast<std::uintptr_t> (hwnd)
      );
   } catch (const winrt::hresult_error& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture,
         "wgc: capture_window failed: {}", winrt::to_string (e.message ())
      ));
   }
}

core::Result<Frame> WgcStrategy::capture_monitor (void* monitor)
{
   if (!impl_->initialized) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "wgc: not initialized"));
   }

   auto hmon = static_cast<HMONITOR> (monitor);

   try {
      auto item = impl_->item_for_monitor (hmon);
      return impl_->capture_item (
         item,
         reinterpret_cast<std::uintptr_t> (hmon),
         0
      );
   } catch (const winrt::hresult_error& e) {
      return core::fail (core::Error::make (
         core::ErrorKind::Capture,
         "wgc: capture_monitor failed: {}", winrt::to_string (e.message ())
      ));
   }
}

std::string_view WgcStrategy::name () const noexcept
{
   if (!impl_) return "wgc";
   return impl_->borderless_supported ? "wgc.borderless" : "wgc.bordered";
}

std::string_view WgcStrategy::reason () const noexcept
{
   return impl_ ? impl_->reason_text : "";
}

bool WgcStrategy::borderless () const noexcept
{
   return impl_ && impl_->borderless_supported;
}

// ---- Continuous capture ---------------------------------------------------

bool WgcStrategy::supports_continuous () const noexcept
{
   return impl_ && impl_->initialized;
}

core::Result<void> WgcStrategy::start_continuous (void* target, bool is_window)
{
   if (!impl_->initialized) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "wgc: not initialized"));
   }

   stop_continuous ();

   try {
      auto item = is_window
         ? impl_->item_for_window  (static_cast<HWND> (target))
         : impl_->item_for_monitor (static_cast<HMONITOR> (target));

      const auto size = item.Size ();

      impl_->cont_pool = Direct3D11CaptureFramePool::CreateFreeThreaded (
         impl_->winrt_device,
         DirectXPixelFormat::B8G8R8A8UIntNormalized,
         2,
         size
      );

      impl_->cont_session = impl_->cont_pool.CreateCaptureSession (item);

      if (impl_->borderless_supported)    impl_->cont_session.IsBorderRequired       (false);
      if (impl_->cursor_toggle_supported) impl_->cont_session.IsCursorCaptureEnabled (false);

      impl_->cont_window_id  = is_window  ? reinterpret_cast<std::uintptr_t> (target) : 0;
      impl_->cont_monitor_id = !is_window ? reinterpret_cast<std::uintptr_t> (target) : 0;
      impl_->cont_active.store (true);

      impl_->cont_token = impl_->cont_pool.FrameArrived (
         [this, is_window] (auto&& sender, auto&&) {
            auto next = sender.TryGetNextFrame ();
            if (!next) return;

            auto surface = next.Surface ();
            auto access  = surface.as<::IDirect3DDxgiInterfaceAccess> ();

            ComPtr<ID3D11Texture2D> source;
            if (FAILED (access->GetInterface (
                  __uuidof (ID3D11Texture2D),
                  reinterpret_cast<void**> (source.GetAddressOf ())))) return;

            D3D11_TEXTURE2D_DESC desc {};
            source->GetDesc (&desc);

            D3D11_TEXTURE2D_DESC staging = desc;
            staging.Usage          = D3D11_USAGE_STAGING;
            staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            staging.BindFlags      = 0;
            staging.MiscFlags      = 0;

            ComPtr<ID3D11Texture2D> staging_tex;
            if (FAILED (impl_->d3d_device->CreateTexture2D (&staging, nullptr, staging_tex.GetAddressOf ()))) return;

            impl_->d3d_context->CopyResource (staging_tex.Get (), source.Get ());

            D3D11_MAPPED_SUBRESOURCE map {};
            if (FAILED (impl_->d3d_context->Map (staging_tex.Get (), 0, D3D11_MAP_READ, 0, &map))) return;

            const int width  = static_cast<int> (desc.Width);
            const int height = static_cast<int> (desc.Height);
            const int stride = static_cast<int> (map.RowPitch);

            auto pixels = std::shared_ptr<std::uint8_t []> (
               new std::uint8_t [ static_cast<std::size_t> (stride) * height ]
            );
            std::memcpy (pixels.get (), map.pData, static_cast<std::size_t> (stride) * height);

            impl_->d3d_context->Unmap (staging_tex.Get (), 0);

            Frame f {
               .data       = std::move (pixels),
               .width      = width,
               .height     = height,
               .stride     = stride,
               .dpi_scale  = 1.0,
               .monitor_id = impl_->cont_monitor_id,
               .window_id  = impl_->cont_window_id,
               .timestamp  = std::chrono::steady_clock::now (),
            };

            {
               std::lock_guard lk { impl_->cont_lock };
               impl_->cont_latest = std::move (f);
            }
            impl_->cont_cv.notify_one ();
         }
      );

      impl_->cont_session.StartCapture ();

      core::Logger::info ("wgc: continuous session started ({}x{}, {})",
         size.Width, size.Height, is_window ? "window" : "monitor");
      return {};
   } catch (const winrt::hresult_error& e) {
      return core::fail (core::Error::make (core::ErrorKind::Capture,
         "wgc: start_continuous failed: {}", winrt::to_string (e.message ())));
   }
}

core::Result<Frame> WgcStrategy::latest_frame (std::chrono::milliseconds timeout)
{
   if (!impl_->cont_active.load ()) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "wgc: continuous not active"));
   }

   std::unique_lock lk { impl_->cont_lock };

   if (!impl_->cont_cv.wait_for (lk, timeout, [this] { return !impl_->cont_latest.empty (); })) {
      return core::fail (core::Error::make (core::ErrorKind::Capture, "wgc: latest_frame timeout"));
   }

   Frame snapshot = impl_->cont_latest;
   impl_->cont_latest = Frame {};        // consume — next caller waits for a fresh frame
   return snapshot;
}

void WgcStrategy::stop_continuous () noexcept
{
   if (!impl_->cont_active.exchange (false)) return;

   try {
      if (impl_->cont_pool)    impl_->cont_pool.FrameArrived (impl_->cont_token);
      if (impl_->cont_session) impl_->cont_session.Close ();
      if (impl_->cont_pool)    impl_->cont_pool.Close ();
   } catch (...) { /* swallow during shutdown */ }

   impl_->cont_pool    = nullptr;
   impl_->cont_session = nullptr;
   impl_->cont_token   = {};
}

} // namespace gv::capture

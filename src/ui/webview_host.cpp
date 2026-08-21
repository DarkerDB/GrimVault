#include <gv/ui/webview_host.h>
#include <gv/core/logger.h>

#include <Windows.h>

// WIN32_LEAN_AND_MEAN (set project-wide) strips COM from Windows.h;
// WebView2.h needs the `interface` macro and IUnknown from these.
#include <objbase.h>
#include <objidl.h>
#include <unknwn.h>

#include <WebView2.h>
#ifdef _MSC_VER
   #pragma push_macro("__has_attribute")
   #undef __has_attribute
#endif
#include <WebView2EnvironmentOptions.h>
#ifdef _MSC_VER
   #pragma pop_macro("__has_attribute")
#endif
#include <fmt/format.h>
#include <wil/com.h>
#include <wrl.h>

#include <algorithm>
#include <cstdint>
#include <string>

using Microsoft::WRL::Callback;

namespace gv::ui {

namespace {

   constexpr wchar_t k_window_class []  = L"GrimVaultAugment";
   constexpr wchar_t k_virtual_host []  = L"grimvault.assets";
   constexpr wchar_t k_start_url []     = L"https://grimvault.assets/augment.html";

   std::string narrow (const wchar_t* wide)
   {
      if (!wide || !*wide) return {};
      const int len = ::WideCharToMultiByte (CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
      if (len <= 0) return {};
      std::string out (static_cast<std::size_t> (len), '\0');
      if (!::WideCharToMultiByte (CP_UTF8, 0, wide, -1, out.data (), len, nullptr, nullptr)) {
         return {};
      }
      out.pop_back (); // API length includes the terminating NUL.
      return out;
   }

   std::wstring widen (const std::string& utf8)
   {
      if (utf8.empty ()) return {};
      const int len = ::MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), -1, nullptr, 0);
      if (len <= 0) return {};
      std::wstring out (static_cast<std::size_t> (len), L'\0');
      if (!::MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), -1, out.data (), len)) {
         return {};
      }
      out.pop_back (); // API length includes the terminating NUL.
      return out;
   }

   LRESULT CALLBACK augment_wndproc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
   {
      // Belt and braces with WS_EX_TRANSPARENT: never hit-test positive, so
      // every click falls through to the game window beneath.
      if (msg == WM_NCHITTEST) return HTTRANSPARENT;
      if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
      // DefWindowProc changes the cursor to the class cursor / system arrow.
      // The overlay never owns input, so preserve the cursor chosen by the
      // game underneath it.
      if (msg == WM_SETCURSOR) return TRUE;
      return ::DefWindowProcW (hwnd, msg, wp, lp);
   }

   ATOM register_window_class ()
   {
      static ATOM atom = [] {
         WNDCLASSEXW wc {};
         wc.cbSize        = sizeof (wc);
         wc.lpfnWndProc   = augment_wndproc;
         wc.hInstance     = ::GetModuleHandleW (nullptr);
         wc.lpszClassName = k_window_class;
         return ::RegisterClassExW (&wc);
      } ();
      return atom;
   }

} // namespace

struct WebviewHost::Impl : std::enable_shared_from_this<WebviewHost::Impl>
{
   Config    config;
   Callbacks callbacks;

   HWND hwnd = nullptr;

   wil::com_ptr<ICoreWebView2Environment> environment;
   wil::com_ptr<ICoreWebView2Controller>  controller;
   wil::com_ptr<ICoreWebView2>            webview;

   bool ready = false;
   bool alive = true;

   HRESULT on_environment (ICoreWebView2Environment* env);
   HRESULT on_controller (ICoreWebView2Controller* value);
   void    on_navigation_completed ();
   void    fail (std::string stage, HRESULT hr);
   void    fail (std::string reason);
   void    teardown ();
};

core::Result<std::string> WebviewHost::runtime_version ()
{
   wil::unique_cotaskmem_string version;
   const HRESULT hr = ::GetAvailableCoreWebView2BrowserVersionString (nullptr, &version);

   if (FAILED (hr) || !version) {
      return core::fail (core::Error {
         core::ErrorKind::NotFound, "WebView2 Evergreen runtime not installed" });
   }

   return narrow (version.get ());
}

WebviewHost::WebviewHost () : impl_ (std::make_shared<Impl> ()) {}

WebviewHost::~WebviewHost ()
{
   // Creation may still be in flight. It owns Impl until its completion
   // callback returns, but must no longer call into the destroyed owner.
   impl_->alive = false;
   impl_->callbacks = {};
   impl_->teardown ();
   impl_.reset ();
}

core::Result<std::unique_ptr<WebviewHost>> WebviewHost::create (Config config,
                                                                Callbacks callbacks)
{
   if (auto v = runtime_version (); !v.has_value ()) {
      return core::fail (v.error ());
   } else {
      core::Logger::info ("webview: runtime {}", *v);
   }

   if (!register_window_class ()) {
      return core::fail (core::Error { core::ErrorKind::Internal, "RegisterClassExW failed" });
   }

   auto host = std::unique_ptr<WebviewHost> { new WebviewHost () };
   const auto impl = host->impl_;
   impl->config    = std::move (config);
   impl->callbacks = std::move (callbacks);

   impl->hwnd = ::CreateWindowExW (
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      k_window_class, L"", WS_POPUP | WS_DISABLED | WS_VISIBLE,
      -32000, -32000, 800, 1600,
      nullptr, nullptr, ::GetModuleHandleW (nullptr), nullptr);

   if (!impl->hwnd) {
      return core::fail (core::Error { core::ErrorKind::Internal, "CreateWindowExW failed" });
   }

   const auto user_data = impl->config.user_data_dir.wstring ();
   Microsoft::WRL::ComPtr<CoreWebView2EnvironmentOptions> options;
   if (impl->config.software_rendering) {
      options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions> ();
      if (!options) {
         impl->alive = false;
         impl->teardown ();
         return core::fail (core::Error {
            core::ErrorKind::Internal, "WebView2 environment options allocation failed" });
      }
      options->put_AdditionalBrowserArguments (L"--disable-gpu");
   }

   core::Logger::info ("webview: creating environment profile='{}' rendering={}",
      impl->config.user_data_dir.string (),
      impl->config.software_rendering ? "software" : "hardware");

   const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions (
      nullptr, user_data.c_str (), options.Get (),
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> (
         [impl] (HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (!impl->alive) return S_OK;
            if (FAILED (result) || !env) {
               impl->fail ("environment", result);
               return result;
            }
            return impl->on_environment (env);
         }).Get ());

   if (FAILED (hr)) {
      impl->alive = false;
      impl->teardown ();
      return core::fail (core::Error { core::ErrorKind::Internal,
         fmt::format ("environment hr=0x{:08x}", static_cast<unsigned long> (hr)) });
   }

   return host;
}

HRESULT WebviewHost::Impl::on_environment (ICoreWebView2Environment* env)
{
   environment = env;

   const auto self = shared_from_this ();
   const HRESULT hr = environment->CreateCoreWebView2Controller (
      hwnd,
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> (
         [self] (HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            if (!self->alive) return S_OK;
            if (FAILED (result) || !controller) {
               self->fail ("controller", result);
               return result;
            }
            return self->on_controller (controller);
         }).Get ());
   if (FAILED (hr)) fail ("controller request", hr);
   return hr;
}

HRESULT WebviewHost::Impl::on_controller (ICoreWebView2Controller* value)
{
   const std::weak_ptr<Impl> weak = shared_from_this ();
   controller = value;
   core::Logger::info ("webview: standard snapshot controller ready");

   if (auto c2 = controller.try_query<ICoreWebView2Controller2> ()) {
      // Alpha 0: the page background composes fully transparent; only the
      // rendered card has pixels.
      c2->put_DefaultBackgroundColor (COREWEBVIEW2_COLOR { 0, 0, 0, 0 });
   }

   if (auto c3 = controller.try_query<ICoreWebView2Controller3> ()) {
      c3->put_BoundsMode (COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
      c3->put_ShouldDetectMonitorScaleChanges (FALSE);
   }

   // A measuring viewport before the first place(): with the default 0x0
   // bounds the page lays out at zero width, the card measures 0, and the
   // size handshake never fires (the window itself stays hidden, so the
   // viewport size is invisible).
   controller->put_Bounds (RECT { 0, 0, 800, 1600 });

   // Controller visibility stays TRUE for the host's life: a FALSE
   // controller suspends layout, so a hidden page never measures and the
   // size handshake never fires. Actual on-screen visibility is the Win32
   // window's alone (ShowWindow); with the window hidden nothing renders
   // regardless.
   controller->put_IsVisible (TRUE);

   HRESULT hr = controller->get_CoreWebView2 (webview.put ());
   if (FAILED (hr) || !webview) {
      fail ("get_CoreWebView2", hr);
      return hr;
   }

   wil::com_ptr<ICoreWebView2Settings> settings;
   if (SUCCEEDED (webview->get_Settings (settings.put ())) && settings) {
      settings->put_AreDefaultContextMenusEnabled (FALSE);
      settings->put_IsStatusBarEnabled (FALSE);
      settings->put_IsZoomControlEnabled (FALSE);
   }

   if (auto wv3 = webview.try_query<ICoreWebView2_3> ()) {
      const auto dir = config.web_dir.wstring ();
      hr = wv3->SetVirtualHostNameToFolderMapping (
         k_virtual_host, dir.c_str (),
         COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
      if (FAILED (hr)) {
         fail ("virtual host mapping", hr);
         return hr;
      }
   } else {
      fail ("ICoreWebView2_3 (virtual host mapping unavailable)", E_NOINTERFACE);
      return E_NOINTERFACE;
   }

   webview->add_WebMessageReceived (
      Callback<ICoreWebView2WebMessageReceivedEventHandler> (
         [weak] (ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            const auto self = weak.lock ();
            if (!self || !self->alive) return S_OK;
            wil::unique_cotaskmem_string json;
            if (SUCCEEDED (args->get_WebMessageAsJson (&json)) && json && self->callbacks.on_message) {
               self->callbacks.on_message (narrow (json.get ()));
            }
            return S_OK;
         }).Get (), nullptr);

   webview->add_NavigationCompleted (
      Callback<ICoreWebView2NavigationCompletedEventHandler> (
         [weak] (ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            const auto self = weak.lock ();
            if (!self || !self->alive) return S_OK;
            BOOL ok = FALSE;
            args->get_IsSuccess (&ok);
            if (!ok) {
               self->fail ("navigation", E_FAIL);
               return S_OK;
            }
            self->on_navigation_completed ();
            return S_OK;
         }).Get (), nullptr);

   webview->add_ProcessFailed (
      Callback<ICoreWebView2ProcessFailedEventHandler> (
         [weak] (ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            const auto self = weak.lock ();
            if (!self || !self->alive) return S_OK;

            COREWEBVIEW2_PROCESS_FAILED_KIND kind {};
            args->get_ProcessFailedKind (&kind);
            auto reason = fmt::format ("process kind={}", static_cast<int> (kind));
            wil::com_ptr<ICoreWebView2ProcessFailedEventArgs2> args2;
            if (SUCCEEDED (args->QueryInterface (IID_PPV_ARGS (args2.put ())))) {
               COREWEBVIEW2_PROCESS_FAILED_REASON process_reason {};
               INT32 exit_code = 0;
               wil::unique_cotaskmem_string description;
               args2->get_Reason (&process_reason);
               args2->get_ExitCode (&exit_code);
               args2->get_ProcessDescription (&description);
               reason = fmt::format ("process kind={} reason={} exit={} description='{}'",
                  static_cast<int> (kind), static_cast<int> (process_reason), exit_code,
                  narrow (description.get ()));
            }
            self->fail (std::move (reason));
            return S_OK;
         }).Get (), nullptr);

   hr = webview->Navigate (k_start_url);
   if (FAILED (hr)) fail ("navigation request", hr);
   return hr;
}

void WebviewHost::Impl::on_navigation_completed ()
{
   if (ready) return;

   ready = true;
   core::Logger::info ("webview: augment page ready");
   if (callbacks.on_ready) callbacks.on_ready ();
}

void WebviewHost::Impl::fail (std::string stage, HRESULT hr)
{
   fail (fmt::format ("{} hr=0x{:08x}", stage,
      static_cast<unsigned long> (hr)));
}

void WebviewHost::Impl::fail (std::string reason)
{
   if (!alive) return;
   alive = false;
   core::Logger::error ("webview: {}", reason);
   const auto notify = callbacks.on_failed;
   teardown ();
   if (notify) notify (std::move (reason));
}

void WebviewHost::Impl::teardown ()
{
   ready = false;

   if (controller) controller->Close ();
   webview     = nullptr;
   controller  = nullptr;
   environment = nullptr;

   if (hwnd) {
      ::DestroyWindow (hwnd);
      hwnd = nullptr;
   }
}

// ---- Public surface ----

bool WebviewHost::ready () const
{
   return impl_->ready;
}

void WebviewHost::post_json (const std::string& json)
{
   if (!impl_->ready || !impl_->webview) {
      core::Logger::debug ("webview: post before ready dropped");
      return;
   }

   impl_->webview->PostWebMessageAsJson (widen (json).c_str ());
}

void WebviewHost::resize (const QSize& physical, double scale)
{
   if (!impl_->hwnd || !impl_->controller || physical.isEmpty ()) return;

   if (auto c3 = impl_->controller.try_query<ICoreWebView2Controller3> ()) {
      c3->put_RasterizationScale (scale);
   }
   ::SetWindowPos (impl_->hwnd, nullptr,
      0, 0, physical.width (), physical.height (),
      SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
   impl_->controller->put_Bounds (
      RECT { 0, 0, physical.width (), physical.height () });
}

void WebviewHost::capture_png (
   std::function<void (std::vector<std::uint8_t>)> callback)
{
   if (!impl_->ready || !impl_->webview) {
      callback ({});
      return;
   }

   wil::com_ptr<IStream> stream;
   const HRESULT stream_hr = ::CreateStreamOnHGlobal (nullptr, TRUE, stream.put ());
   if (FAILED (stream_hr)) {
      core::Logger::error ("webview: snapshot stream failed hr=0x{:08x}",
         static_cast<unsigned long> (stream_hr));
      callback ({});
      return;
   }

   const std::weak_ptr<Impl> weak = impl_;
   auto done = std::make_shared<std::function<void (std::vector<std::uint8_t>)>> (
      std::move (callback));
   const HRESULT hr = impl_->webview->CapturePreview (
      COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG,
      stream.get (),
      Callback<ICoreWebView2CapturePreviewCompletedHandler> (
         [weak, stream, done] (HRESULT result) mutable -> HRESULT {
            if (FAILED (result) || weak.expired ()) {
               if (FAILED (result)) {
                  core::Logger::error ("webview: snapshot callback failed hr=0x{:08x}",
                     static_cast<unsigned long> (result));
               }
               (*done) ({});
               return S_OK;
            }

            HGLOBAL memory = nullptr;
            if (FAILED (::GetHGlobalFromStream (stream.get (), &memory)) || !memory) {
               core::Logger::error ("webview: snapshot stream extraction failed");
               (*done) ({});
               return S_OK;
            }

            const auto size = static_cast<std::size_t> (::GlobalSize (memory));
            const auto* data = static_cast<const std::uint8_t*> (::GlobalLock (memory));
            if (!data || size == 0) {
               core::Logger::error ("webview: snapshot stream empty size={}", size);
               if (data) ::GlobalUnlock (memory);
               (*done) ({});
               return S_OK;
            }

            std::vector<std::uint8_t> png (data, data + size);
            ::GlobalUnlock (memory);
            (*done) (std::move (png));
            return S_OK;
         }).Get ());

   if (FAILED (hr)) {
      core::Logger::error ("webview: CapturePreview failed hr=0x{:08x}",
         static_cast<unsigned long> (hr));
      (*done) ({});
   }
}

} // namespace gv::ui

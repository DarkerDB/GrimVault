#include <gv/ui/webview_host.h>
#include <gv/core/logger.h>

#include <Windows.h>

// WIN32_LEAN_AND_MEAN (set project-wide) strips COM from Windows.h;
// WebView2.h needs the `interface` macro and IUnknown from these.
#include <objbase.h>
#include <objidl.h>
#include <unknwn.h>

#include <WebView2.h>
#include <dcomp.h>
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

   wil::com_ptr<IDCompositionDevice>       dcomp_device;
   wil::com_ptr<IDCompositionTarget>       dcomp_target;
   wil::com_ptr<IDCompositionVisual>       dcomp_visual;

   wil::com_ptr<ICoreWebView2Environment>           environment;
   wil::com_ptr<ICoreWebView2CompositionController> composition;
   wil::com_ptr<ICoreWebView2Controller>            controller;
   wil::com_ptr<ICoreWebView2>                      webview;

   bool ready   = false;
   bool visible = false;

   HRESULT on_environment (ICoreWebView2Environment* env);
   HRESULT on_composition_controller (ICoreWebView2CompositionController* comp);
   void    on_navigation_completed ();
   void    fail (const char* stage, HRESULT hr);
   void    teardown ();
};

// ---- Creation chain ----
//
// create() -> CreateCoreWebView2EnvironmentWithOptions
//          -> on_environment: CreateCoreWebView2CompositionController
//          -> on_composition_controller: wire visual target, background,
//             raw-pixel bounds, virtual host, message handler; Navigate
//          -> NavigationCompleted: ready

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

   // WS_EX_NOREDIRECTIONBITMAP: content arrives via DComp only, no GDI
   // redirection surface (the HWND-mode flicker source over games).
   //
   // WS_DISABLED is intentional. NOACTIVATE prevents focus changes but still
   // leaves a top-level window eligible for mouse hit selection; that is
   // enough to interrupt the game's hover state. Disabled windows are skipped
   // by Windows point selection and cannot receive mouse/keyboard input, while
   // their DirectComposition visual tree remains visible.
   impl->hwnd = ::CreateWindowExW (
      WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
    | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      k_window_class, L"", WS_POPUP | WS_DISABLED,
      0, 0, 1, 1,
      nullptr, nullptr, ::GetModuleHandleW (nullptr), nullptr);

   if (!impl->hwnd) {
      return core::fail (core::Error { core::ErrorKind::Internal, "CreateWindowExW failed" });
   }

   HRESULT hr = ::DCompositionCreateDevice2 (
      nullptr, IID_PPV_ARGS (impl->dcomp_device.put ()));
   if (SUCCEEDED (hr)) {
      hr = impl->dcomp_device->CreateTargetForHwnd (
         impl->hwnd, TRUE, impl->dcomp_target.put ());
   }
   if (SUCCEEDED (hr)) {
      hr = impl->dcomp_device->CreateVisual (impl->dcomp_visual.put ());
   }
   if (SUCCEEDED (hr)) {
      hr = impl->dcomp_target->SetRoot (impl->dcomp_visual.get ());
   }
   if (FAILED (hr)) {
      impl->teardown ();
      return core::fail (core::Error { core::ErrorKind::Internal, "DirectComposition setup failed" });
   }

   const auto user_data = impl->config.user_data_dir.wstring ();

   hr = ::CreateCoreWebView2EnvironmentWithOptions (
      nullptr, user_data.c_str (), nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> (
         [impl] (HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED (result) || !env) {
               impl->fail ("environment", result);
               return result;
            }
            return impl->on_environment (env);
         }).Get ());

   if (FAILED (hr)) {
      impl->teardown ();
      return core::fail (core::Error { core::ErrorKind::Internal, "CreateCoreWebView2Environment failed" });
   }

   return host;
}

HRESULT WebviewHost::Impl::on_environment (ICoreWebView2Environment* env)
{
   environment = env;

   auto env10 = environment.try_query<ICoreWebView2Environment10> ();
   if (!env10) {
      fail ("environment10 (runtime too old for host input processing)", E_NOINTERFACE);
      return E_NOINTERFACE;
   }

   wil::com_ptr<ICoreWebView2ControllerOptions> options;
   HRESULT hr = env10->CreateCoreWebView2ControllerOptions (options.put ());
   if (FAILED (hr) || !options) {
      fail ("CreateCoreWebView2ControllerOptions", hr);
      return FAILED (hr) ? hr : E_FAIL;
   }

   auto options4 = options.try_query<ICoreWebView2ControllerOptions4> ();
   if (!options4) {
      fail ("controller options4 (AllowHostInputProcessing unavailable)", E_NOINTERFACE);
      return E_NOINTERFACE;
   }
   hr = options4->put_AllowHostInputProcessing (TRUE);
   if (FAILED (hr)) {
      fail ("put_AllowHostInputProcessing", hr);
      return hr;
   }

   const auto self = shared_from_this ();
   return env10->CreateCoreWebView2CompositionControllerWithOptions (
      hwnd, options.get (),
      Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler> (
         [self] (HRESULT result, ICoreWebView2CompositionController* comp) -> HRESULT {
            if (FAILED (result) || !comp) {
               self->fail ("composition controller", result);
               return result;
            }
            return self->on_composition_controller (comp);
         }).Get ());
}

HRESULT WebviewHost::Impl::on_composition_controller (ICoreWebView2CompositionController* comp)
{
   const std::weak_ptr<Impl> weak = shared_from_this ();
   composition = comp;
   controller  = composition.query<ICoreWebView2Controller> ();

   HRESULT hr = composition->put_RootVisualTarget (dcomp_visual.get ());
   if (FAILED (hr)) {
      fail ("put_RootVisualTarget", hr);
      return hr;
   }

   // DirectComposition batches tree changes; without a Commit the visual
   // (now bound to WebView2's content) never reaches the screen. The page
   // renders and measures, but nothing is ever presented.
   dcomp_device->Commit ();

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

   hr = controller->get_CoreWebView2 (webview.put ());
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
      wv3->SetVirtualHostNameToFolderMapping (
         k_virtual_host, dir.c_str (),
         COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
   } else {
      fail ("ICoreWebView2_3 (virtual host mapping unavailable)", E_NOINTERFACE);
      return E_NOINTERFACE;
   }

   webview->add_WebMessageReceived (
      Callback<ICoreWebView2WebMessageReceivedEventHandler> (
         [weak] (ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            const auto self = weak.lock ();
            if (!self) return S_OK;
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
            if (!self) return S_OK;
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
         [weak] (ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            const auto self = weak.lock ();
            if (!self) return S_OK;
            core::Logger::warn ("webview: browser process failed; tearing down");
            const auto notify = self->callbacks.on_process_failed;
            self->teardown ();
            if (notify) notify ();
            return S_OK;
         }).Get (), nullptr);

   return webview->Navigate (k_start_url);
}

void WebviewHost::Impl::on_navigation_completed ()
{
   if (ready) return;

   ready = true;
   core::Logger::info ("webview: augment page ready");
   if (callbacks.on_ready) callbacks.on_ready ();
}

void WebviewHost::Impl::fail (const char* stage, HRESULT hr)
{
   core::Logger::error ("webview: {} failed (hr=0x{:08x})", stage,
      static_cast<unsigned long> (hr));

   const auto notify = callbacks.on_process_failed;
   teardown ();
   if (notify) notify ();
}

void WebviewHost::Impl::teardown ()
{
   ready   = false;
   visible = false;

   if (controller) controller->Close ();
   webview      = nullptr;
   controller   = nullptr;
   composition  = nullptr;
   environment  = nullptr;
   dcomp_visual = nullptr;
   dcomp_target = nullptr;
   dcomp_device = nullptr;

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

void WebviewHost::place (const QRect& physical, double scale)
{
   if (!impl_->hwnd || !impl_->controller) return;

   if (auto c3 = impl_->controller.try_query<ICoreWebView2Controller3> ()) {
      c3->put_RasterizationScale (scale);
   }

   ::SetWindowPos (impl_->hwnd, HWND_TOPMOST,
      physical.x (), physical.y (), physical.width (), physical.height (),
      SWP_NOACTIVATE);

   impl_->controller->put_Bounds (
      RECT { 0, 0, physical.width (), physical.height () });
}

void WebviewHost::resize (const QSize& physical, double scale)
{
   if (!impl_->controller || physical.isEmpty ()) return;

   if (auto c3 = impl_->controller.try_query<ICoreWebView2Controller3> ()) {
      c3->put_RasterizationScale (scale);
   }
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
   if (FAILED (::CreateStreamOnHGlobal (nullptr, TRUE, stream.put ()))) {
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
               (*done) ({});
               return S_OK;
            }

            HGLOBAL memory = nullptr;
            if (FAILED (::GetHGlobalFromStream (stream.get (), &memory)) || !memory) {
               (*done) ({});
               return S_OK;
            }

            const auto size = static_cast<std::size_t> (::GlobalSize (memory));
            const auto* data = static_cast<const std::uint8_t*> (::GlobalLock (memory));
            if (!data || size == 0) {
               if (data) ::GlobalUnlock (memory);
               (*done) ({});
               return S_OK;
            }

            std::vector<std::uint8_t> png (data, data + size);
            ::GlobalUnlock (memory);
            (*done) (std::move (png));
            return S_OK;
         }).Get ());

   if (FAILED (hr)) (*done) ({});
}

void WebviewHost::move (const QPoint& physical)
{
   if (!impl_->hwnd) return;

   ::SetWindowPos (impl_->hwnd, HWND_TOPMOST,
      physical.x (), physical.y (), 0, 0,
      SWP_NOSIZE | SWP_NOACTIVATE);
}

void WebviewHost::show ()
{
   if (!impl_->hwnd || impl_->visible) return;

   // SetWindowPos over ShowWindow: a WS_EX_NOREDIRECTIONBITMAP tool window
   // does not reliably take WS_VISIBLE from ShowWindow (SW_SHOWNOACTIVATE
   // reports success but IsWindowVisible stays false). SWP_SHOWWINDOW sets
   // the style bit directly and keeps the topmost band.
   ::SetWindowPos (impl_->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
   impl_->visible = true;
}

void WebviewHost::hide ()
{
   if (!impl_->hwnd || !impl_->visible) return;

   // Window only: the controller stays visible so layout keeps running
   // and the page can re-measure while off-screen.
   ::SetWindowPos (impl_->hwnd, nullptr, 0, 0, 0, 0,
      SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER | SWP_HIDEWINDOW);
   impl_->visible = false;
}

} // namespace gv::ui

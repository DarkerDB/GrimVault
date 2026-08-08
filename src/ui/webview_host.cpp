#include <gv/ui/webview_host.h>
#include <gv/core/logger.h>

#include <Windows.h>

// WIN32_LEAN_AND_MEAN (set project-wide) strips COM from Windows.h;
// WebView2.h needs the `interface` macro and IUnknown from these.
#include <objbase.h>
#include <unknwn.h>

#include <WebView2.h>
#include <dcomp.h>
#include <wil/com.h>
#include <wrl.h>

#include <utility>

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
      std::string out (static_cast<std::size_t> (len > 0 ? len - 1 : 0), '\0');
      ::WideCharToMultiByte (CP_UTF8, 0, wide, -1, out.data (), len, nullptr, nullptr);
      return out;
   }

   std::wstring widen (const std::string& utf8)
   {
      if (utf8.empty ()) return {};
      const int len = ::MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), -1, nullptr, 0);
      std::wstring out (static_cast<std::size_t> (len > 0 ? len - 1 : 0), L'\0');
      ::MultiByteToWideChar (CP_UTF8, 0, utf8.c_str (), -1, out.data (), len);
      return out;
   }

   LRESULT CALLBACK augment_wndproc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
   {
      // Belt and braces with WS_EX_TRANSPARENT: never hit-test positive, so
      // every click falls through to the game window beneath.
      if (msg == WM_NCHITTEST) return HTTRANSPARENT;
      if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
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

struct WebviewHost::Impl
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

WebviewHost::WebviewHost () : impl_ (std::make_unique<Impl> ()) {}

WebviewHost::~WebviewHost ()
{
   impl_->teardown ();
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
   auto* impl = host->impl_.get ();
   impl->config    = std::move (config);
   impl->callbacks = std::move (callbacks);

   // WS_EX_NOREDIRECTIONBITMAP: content arrives via DComp only, no GDI
   // redirection surface (the HWND-mode flicker source over games).
   impl->hwnd = ::CreateWindowExW (
      WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW
    | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
      k_window_class, L"", WS_POPUP,
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

   auto env3 = environment.try_query<ICoreWebView2Environment3> ();
   if (!env3) {
      fail ("environment3 (runtime too old for composition hosting)", E_NOINTERFACE);
      return E_NOINTERFACE;
   }

   return env3->CreateCoreWebView2CompositionController (
      hwnd,
      Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler> (
         [this] (HRESULT result, ICoreWebView2CompositionController* comp) -> HRESULT {
            if (FAILED (result) || !comp) {
               fail ("composition controller", result);
               return result;
            }
            return on_composition_controller (comp);
         }).Get ());
}

HRESULT WebviewHost::Impl::on_composition_controller (ICoreWebView2CompositionController* comp)
{
   composition = comp;
   controller  = composition.query<ICoreWebView2Controller> ();

   HRESULT hr = composition->put_RootVisualTarget (dcomp_visual.get ());
   if (FAILED (hr)) {
      fail ("put_RootVisualTarget", hr);
      return hr;
   }

   if (auto c2 = controller.try_query<ICoreWebView2Controller2> ()) {
      // Alpha 0: the page background composes fully transparent; only the
      // rendered card has pixels.
      c2->put_DefaultBackgroundColor (COREWEBVIEW2_COLOR { 0, 0, 0, 0 });
   }

   if (auto c3 = controller.try_query<ICoreWebView2Controller3> ()) {
      c3->put_BoundsMode (COREWEBVIEW2_BOUNDS_MODE_USE_RAW_PIXELS);
      c3->put_ShouldDetectMonitorScaleChanges (FALSE);
   }

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
         [this] (ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            wil::unique_cotaskmem_string json;
            if (SUCCEEDED (args->get_WebMessageAsJson (&json)) && json && callbacks.on_message) {
               callbacks.on_message (narrow (json.get ()));
            }
            return S_OK;
         }).Get (), nullptr);

   webview->add_NavigationCompleted (
      Callback<ICoreWebView2NavigationCompletedEventHandler> (
         [this] (ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL ok = FALSE;
            args->get_IsSuccess (&ok);
            if (!ok) {
               fail ("navigation", E_FAIL);
               return S_OK;
            }
            on_navigation_completed ();
            return S_OK;
         }).Get (), nullptr);

   webview->add_ProcessFailed (
      Callback<ICoreWebView2ProcessFailedEventHandler> (
         [this] (ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
            core::Logger::warn ("webview: browser process failed; tearing down");
            const auto notify = callbacks.on_process_failed;
            teardown ();
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

void WebviewHost::show ()
{
   if (!impl_->hwnd || impl_->visible) return;

   ::ShowWindow (impl_->hwnd, SW_SHOWNOACTIVATE);
   if (impl_->controller) impl_->controller->put_IsVisible (TRUE);
   impl_->visible = true;
}

void WebviewHost::hide ()
{
   if (!impl_->hwnd || !impl_->visible) return;

   ::ShowWindow (impl_->hwnd, SW_HIDE);
   if (impl_->controller) impl_->controller->put_IsVisible (FALSE);
   impl_->visible = false;
}

} // namespace gv::ui

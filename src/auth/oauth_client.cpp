#include <gv/auth/oauth_client.h>

#include <gv/auth/http.h>
#include <gv/auth/loopback_server.h>
#include <gv/auth/pkce.h>
#include <gv/core/logger.h>

#include <QCoreApplication>
#include <QDesktopServices>
#include <QString>
#include <QUrl>

#ifdef _WIN32
   #include <Windows.h>
   #include <shellapi.h>
   #pragma comment (lib, "Shell32.lib")
#endif

#include <nlohmann/json.hpp>

#include <chrono>
#include <sstream>
#include <string>

namespace gv::auth {

namespace {

   // RFC 3986 §2.3 + 2.4. Reserved chars (except unreserved) get %HH-escaped.
   std::string url_escape (std::string_view s)
   {
      static constexpr char hex [] = "0123456789ABCDEF";
      std::string out;
      out.reserve (s.size ());
      for (unsigned char c : s) {
         const bool unreserved =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '.' || c == '_' || c == '~';
         if (unreserved) { out.push_back (static_cast<char> (c)); continue; }
         out.push_back ('%');
         out.push_back (hex [c >> 4]);
         out.push_back (hex [c & 0x0F]);
      }
      return out;
   }

   std::string form_encode (
      std::initializer_list<std::pair<std::string_view, std::string_view>> pairs)
   {
      std::string out;
      bool first = true;
      for (const auto& [k, v] : pairs) {
         if (!first) out.push_back ('&');
         first = false;
         out += url_escape (k);
         out.push_back ('=');
         out += url_escape (v);
      }
      return out;
   }

   TokenSet parse_token_response (const nlohmann::json& json)
   {
      // The KATforge envelope wraps the RFC 6749 payload under "body";
      // tolerate both shapes so a raw RFC response still parses.
      const nlohmann::json* src = &json;

      if (json.is_object ()) {
         if (auto it = json.find ("body"); it != json.end () && it->is_object ()) {
            src = &*it;
         }
      }

      const nlohmann::json& j = *src;

      TokenSet t;
      t.access_token  = j.value ("access_token",  "");
      t.refresh_token = j.value ("refresh_token", "");
      t.scope         = j.value ("scope",         "");
      const auto expires_in = j.value ("expires_in", 0);
      const auto skew = std::chrono::seconds { 60 };
      t.expires_at = std::chrono::system_clock::now ()
                   + std::chrono::seconds { expires_in }
                   - skew;
      return t;
   }

} // namespace

struct OauthClient::Impl
{
   Config       cfg;
   BrowserHook  browser_hook;
};

OauthClient::OauthClient (Config cfg)
   : impl_ (std::make_unique<Impl> ())
{
   impl_->cfg = std::move (cfg);
}

OauthClient::~OauthClient () = default;

void OauthClient::set_browser_hook (BrowserHook hook)
{
   impl_->browser_hook = std::move (hook);
}

core::Result<TokenResponse> OauthClient::authorize ()
{
   LoopbackServer server;
   auto port = server.bind ();
   if (!port.has_value ()) return core::fail (port.error ());

   const auto pkce  = pkce_generate ();
   const auto state = state_generate ();

   const std::string redirect_uri =
      "http://127.0.0.1:" + std::to_string (*port) + "/callback";

   std::ostringstream url;
   url << impl_->cfg.auth_base_url << "/oauth/authorize"
       << "?response_type=code"
       << "&client_id="             << url_escape (impl_->cfg.client_id)
       << "&redirect_uri="          << url_escape (redirect_uri)
       << "&scope="                 << url_escape (impl_->cfg.scope)
       << "&state="                 << url_escape (state)
       << "&code_challenge="        << url_escape (pkce.challenge)
       << "&code_challenge_method=S256";

   const std::string authorize_url = url.str ();
   core::log::api.info ("oauth: opening browser to {}", authorize_url);

   bool launch_browser = true;
   if (impl_->browser_hook) {
      launch_browser = impl_->browser_hook (authorize_url);
   }
   if (launch_browser) {
      const QString qurl = QString::fromStdString (authorize_url);

      // QDesktopServices::openUrl prints a noisy warning and refuses to
      // run under a bare QCoreApplication (CLI mode), so check for a
      // QGuiApplication before invoking it. CLI mode falls straight
      // through to the ShellExecuteW path on Windows.
      const auto* qapp = QCoreApplication::instance ();
      const bool is_gui_app = qapp && qapp->inherits ("QGuiApplication");

      bool opened = false;
      if (is_gui_app) {
         opened = QDesktopServices::openUrl (QUrl (qurl));
      }
#ifdef _WIN32
      if (!opened) {
         const auto wurl = qurl.toStdWString ();
         const auto rc = reinterpret_cast<INT_PTR> (
            ::ShellExecuteW (nullptr, L"open", wurl.c_str (),
                             nullptr, nullptr, SW_SHOWNORMAL));
         opened = (rc > 32);
      }
#endif
      if (!opened) {
         core::log::api.warn ("oauth: browser launch failed (url={})", authorize_url);
      }
   }

   // Branded post-callback pages on the realm SPA. The loopback server
   // emits a 302 here instead of its own static HTML when the URL is
   // non-empty.
   std::string success_redirect;
   std::string error_redirect;
   if (!impl_->cfg.spa_base_url.empty ()) {
      success_redirect = impl_->cfg.spa_base_url + "/grimvault/callback?status=ok";
      error_redirect   = impl_->cfg.spa_base_url + "/grimvault/callback?status=error";
   }

   auto cb = server.await_callback (state, impl_->cfg.callback_timeout,
                                    success_redirect, error_redirect);
   if (!cb.has_value ()) return core::fail (cb.error ());

   // Exchange the code at /oauth/token.
   http::Request req;
   req.method       = "POST";
   req.url          = impl_->cfg.api_base_url + "/oauth/token";
   req.body         = form_encode ({
      { "grant_type",    "authorization_code" },
      { "code",          cb->code },
      { "redirect_uri",  redirect_uri },
      { "client_id",     impl_->cfg.client_id },
      { "code_verifier", pkce.verifier },
   });
   req.content_type = "application/x-www-form-urlencoded";

   auto res = http::perform (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "Token exchange failed (HTTP {}): {}", res->status,
         res->body.substr (0, 300)));
   }

   auto json = nlohmann::json::parse (res->body, nullptr, false);
   if (json.is_discarded ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "Token response from the server wasn't valid JSON."));
   }

   auto tokens = parse_token_response (json);
   if (tokens.access_token.empty () || tokens.refresh_token.empty ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "Token response parsed but {} missing — body: {}",
         tokens.access_token.empty () ? "access_token" : "refresh_token",
         res->body.substr (0, 300)));
   }
   return TokenResponse { std::move (tokens) };
}

core::Result<TokenResponse> OauthClient::refresh (std::string_view refresh_token)
{
   http::Request req;
   req.method       = "POST";
   req.url          = impl_->cfg.api_base_url + "/oauth/token";
   req.body         = form_encode ({
      { "grant_type",    "refresh_token" },
      { "refresh_token", refresh_token },
      { "client_id",     impl_->cfg.client_id },
   });
   req.content_type = "application/x-www-form-urlencoded";

   auto res = http::perform (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status == 400 || res->status == 401) {
      // invalid_grant — refresh token dead. Fatal.
      return core::fail (core::Error::make (core::ErrorKind::Permission,
         "oauth: refresh rejected HTTP {}: {}", res->status,
         res->body.substr (0, 300)));
   }
   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "oauth: refresh failed HTTP {}: {}", res->status,
         res->body.substr (0, 300)));
   }

   auto json = nlohmann::json::parse (res->body, nullptr, false);
   if (json.is_discarded ()) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "oauth: refresh response not JSON"));
   }

   auto t = parse_token_response (json);
   if (t.refresh_token.empty ()) {
      // Contract §3.3: rotation is required. Surface as hard failure so the
      // operator sees that KATforge regressed on this guarantee.
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "oauth: refresh response missing rotated refresh_token"));
   }
   return TokenResponse { std::move (t) };
}

core::Result<void> OauthClient::revoke (std::string_view refresh_token)
{
   http::Request req;
   req.method       = "POST";
   req.url          = impl_->cfg.api_base_url + "/oauth/revoke";
   req.body         = form_encode ({
      { "token",           refresh_token },
      { "token_type_hint", "refresh_token" },
      { "client_id",       impl_->cfg.client_id },
   });
   req.content_type = "application/x-www-form-urlencoded";

   auto res = http::perform (req);
   if (!res.has_value ()) return core::fail (res.error ());

   if (res->status < 200 || res->status >= 300) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "oauth: revoke HTTP {}: {}", res->status, res->body.substr (0, 200)));
   }
   return {};
}

} // namespace gv::auth

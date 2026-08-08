#include <gv/core/http.h>

#include <gv/core/env_resolver.h>
#include <gv/core/environment.h>
#include <gv/core/logger.h>

#include <chrono>
#include <cstddef>
#include <limits>

namespace gv::core::http {

namespace {

   struct WriteState {
      std::string* body     = nullptr;
      std::size_t  limit    = 0;
      bool         exceeded = false;
   };

   std::size_t write_cb (char* ptr, std::size_t size, std::size_t nmemb, void* user)
   {
      if (size != 0 && nmemb > std::numeric_limits<std::size_t>::max () / size) return 0;

      const std::size_t n = size * nmemb;
      auto* state = static_cast<WriteState*> (user);
      if (!state || !state->body || state->body->size () > state->limit
          || n > state->limit - state->body->size ()) {
         if (state) state->exceeded = true;
         return 0;
      }
      state->body->append (ptr, n);
      return n;
   }

} // namespace

Global::Global ()
   : initialized_ (curl_global_init (CURL_GLOBAL_DEFAULT) == CURLE_OK)
{}

Global::~Global ()
{
   if (initialized_) curl_global_cleanup ();
}

void apply_tls (CURL* curl, const std::string& ca_bundle)
{
   const auto insecure = environment::get ("GRIMVAULT_INSECURE_DEV_TLS");
   const bool  explicitly_insecure = active_env ().name == "dev"
      && !insecure.empty () && insecure != "0";
   const bool  strict_tls = !explicitly_insecure;

   curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER, strict_tls ? 1L : 0L);
   curl_easy_setopt (curl, CURLOPT_SSL_VERIFYHOST, strict_tls ? 2L : 0L);
#ifdef _WIN32
   // The vcpkg build uses OpenSSL, whose bundled roots do not include the
   // locally installed mkcert issuer trusted by Windows and the browser.
   // Merge the Windows certificate store so dev certificates and managed
   // enterprise roots work without weakening verification.
   curl_easy_setopt (curl, CURLOPT_SSL_OPTIONS,
      CURLSSLOPT_NATIVE_CA | CURLSSLOPT_REVOKE_BEST_EFFORT);
#endif

   if (!ca_bundle.empty ()) {
      curl_easy_setopt (curl, CURLOPT_CAINFO, ca_bundle.c_str ());
   }
}

Result<Response> perform (const Request& req)
{
   CURL* curl = curl_easy_init ();

   if (!curl) {
      return fail (Error::make (ErrorKind::ExternalApi, "http: curl_easy_init failed"));
   }

   curl_slist* headers = nullptr;

   for (const auto& h : req.headers) {
      const auto line = h.name + ": " + h.value;
      headers = curl_slist_append (headers, line.c_str ());
   }

   if (!req.body.empty () && !req.content_type.empty ()) {
      const auto line = std::string { "Content-Type: " } + req.content_type;
      headers = curl_slist_append (headers, line.c_str ());
   }

   headers = curl_slist_append (headers, "Accept: application/json");

   Response res;
   WriteState write_state { &res.body, req.max_response_bytes, false };
   char     err_buf [CURL_ERROR_SIZE] { 0 };

   curl_easy_setopt (curl, CURLOPT_ERRORBUFFER,       err_buf);
   curl_easy_setopt (curl, CURLOPT_URL,               req.url.c_str ());
   curl_easy_setopt (curl, CURLOPT_HTTPHEADER,        headers);
   curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION,     &write_cb);
   curl_easy_setopt (curl, CURLOPT_WRITEDATA,         &write_state);
   curl_easy_setopt (curl, CURLOPT_TIMEOUT_MS,        static_cast<long> (req.timeout.count ()));
   curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
   curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION,    req.follow_redirects ? 1L : 0L);
   curl_easy_setopt (curl, CURLOPT_NOSIGNAL,          1L);
   apply_tls (curl, req.ca_bundle);

   if (req.method == "POST") {
      curl_easy_setopt (curl, CURLOPT_POST,          1L);
      curl_easy_setopt (curl, CURLOPT_POSTFIELDS,    req.body.c_str ());
      curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE, static_cast<long> (req.body.size ()));
   } else if (req.method != "GET") {
      curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, req.method.c_str ());
   }

   const auto     t0 = std::chrono::steady_clock::now ();
   const CURLcode rc = curl_easy_perform (curl);
   curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &res.status);
   res.elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::steady_clock::now () - t0);

   curl_slist_free_all (headers);
   curl_easy_cleanup   (curl);

   if (rc != CURLE_OK) {
      if (rc == CURLE_WRITE_ERROR && write_state.exceeded) {
         return fail (Error::make (ErrorKind::ExternalApi,
            "http: response exceeded {} bytes", req.max_response_bytes));
      }
      // err_buf carries the schannel/openssl-specific reason; fall back
      // to the generic per-code message when libcurl didn't populate it.
      const std::string detail = err_buf [0] ? err_buf : curl_easy_strerror (rc);
      log::api.event ("http.error", {
         { "method",   req.method },
         { "url",      req.url },
         { "curl_err", curl_easy_strerror (rc) },
         { "detail",   detail },
      });
      return fail (Error::make (ErrorKind::ExternalApi,
         "http: curl failed (code {}): {}", static_cast<int> (rc), detail));
   }

   log::api.event ("http.request", {
      { "method", req.method },
      { "url",    req.url },
      { "status", std::to_string (res.status) },
      { "ms",     std::to_string (res.elapsed.count ()) },
   });
   return res;
}

} // namespace gv::core::http

#include <gv/auth/http.h>

#include <gv/core/logger.h>

#include <curl/curl.h>

#include <chrono>

namespace gv::auth::http {

namespace {

   std::size_t write_cb (char* ptr, std::size_t size, std::size_t nmemb, void* user)
   {
      const std::size_t n = size * nmemb;
      auto* out = static_cast<std::string*> (user);
      out->append (ptr, n);
      return n;
   }

} // namespace

core::Result<Response> perform (const Request& req)
{
   CURL* curl = curl_easy_init ();
   if (!curl) {
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "http: curl_easy_init failed"));
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
   curl_easy_setopt (curl, CURLOPT_URL,               req.url.c_str ());
   curl_easy_setopt (curl, CURLOPT_HTTPHEADER,        headers);
   curl_easy_setopt (curl, CURLOPT_WRITEFUNCTION,     &write_cb);
   curl_easy_setopt (curl, CURLOPT_WRITEDATA,         &res.body);
   curl_easy_setopt (curl, CURLOPT_TIMEOUT_MS,        static_cast<long> (req.timeout.count ()));
   curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
   curl_easy_setopt (curl, CURLOPT_FOLLOWLOCATION,    1L);
   curl_easy_setopt (curl, CURLOPT_NOSIGNAL,          1L);
   curl_easy_setopt (curl, CURLOPT_SSL_VERIFYPEER,    1L);
   curl_easy_setopt (curl, CURLOPT_SSL_VERIFYHOST,    2L);

   if (req.method == "POST") {
      curl_easy_setopt (curl, CURLOPT_POST,          1L);
      curl_easy_setopt (curl, CURLOPT_POSTFIELDS,    req.body.c_str ());
      curl_easy_setopt (curl, CURLOPT_POSTFIELDSIZE, static_cast<long> (req.body.size ()));
   } else if (req.method != "GET") {
      curl_easy_setopt (curl, CURLOPT_CUSTOMREQUEST, req.method.c_str ());
   }

   const auto t0 = std::chrono::steady_clock::now ();
   const CURLcode rc = curl_easy_perform (curl);
   curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &res.status);
   res.elapsed = std::chrono::duration_cast<std::chrono::milliseconds> (
      std::chrono::steady_clock::now () - t0);

   curl_slist_free_all (headers);
   curl_easy_cleanup   (curl);

   if (rc != CURLE_OK) {
      core::log::api.event ("http.error", {
         { "method",   req.method },
         { "url",      req.url },
         { "curl_err", curl_easy_strerror (rc) },
      });
      return core::fail (core::Error::make (core::ErrorKind::ExternalApi,
         "http: curl failed: {}", curl_easy_strerror (rc)));
   }

   core::log::api.event ("http.request", {
      { "method", req.method },
      { "url",    req.url },
      { "status", std::to_string (res.status) },
      { "ms",     std::to_string (res.elapsed.count ()) },
   });
   return res;
}

} // namespace gv::auth::http

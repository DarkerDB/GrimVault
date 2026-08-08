#pragma once

#include <gv/core/result.h>

#include <curl/curl.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace gv::core::http {

struct Header {
   std::string name;
   std::string value;
};

struct Response {
   long                      status   = 0;
   std::string               body;
   std::chrono::milliseconds elapsed { 0 };
};

struct Request {
   std::string         method;       // "GET" / "POST"
   std::string         url;
   std::string         body;         // empty = no body
   std::string         content_type; // applied only if body non-empty
   std::string         ca_bundle;    // empty = system trust store
   std::vector<Header> headers;
   std::chrono::milliseconds timeout { 15000 };
   std::size_t         max_response_bytes { 1024 * 1024 };
   bool                follow_redirects   = false;
};

// Process-scoped libcurl lifetime. Construct this before every object that
// may own or use an easy handle. Reverse destruction then guarantees global
// cleanup runs only after all HTTP clients and worker threads are gone.
class Global final
{
public:
   Global ();
   ~Global ();

   Global (const Global&) = delete;
   Global& operator= (const Global&) = delete;

   explicit operator bool () const noexcept { return initialized_; }

private:
   bool initialized_ = false;
};

// TLS verification policy, shared by perform () below and by the persistent
// -handle lane in DDBClient so the two cannot drift apart.
//
// Verification is on everywhere by default. Turning it off needs BOTH
// env=dev AND GRIMVAULT_INSECURE_DEV_TLS set to something other than "0",
// so a dev build pointed at prod still verifies. Windows builds merge the
// native certificate store into OpenSSL trust, covering local development
// and managed enterprise roots. Revocation is best-effort: a CRL/OCSP
// endpoint that cannot be reached is not fatal, but revoked certificates are.
void apply_tls (CURL* curl, const std::string& ca_bundle);

// One-shot libcurl request. DDBClient keeps its own persistent handle, retry
// ladder and bounded response reader; everything else (OAuth token exchange,
// doctor probes) comes through here.
//
// Responses are bounded and redirects are disabled by default. OAuth token
// posts must never forward credentials to a redirected origin.
Result<Response> perform (const Request& req);

} // namespace gv::core::http

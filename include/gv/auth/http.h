#pragma once

#include <gv/core/result.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace gv::auth::http {

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
   std::vector<Header> headers;
   std::chrono::milliseconds timeout { 15000 };
};

// One-shot libcurl wrapper used by the OAuth + bearer-authed API paths.
// Caller is responsible for global_init / global_cleanup; DDBClient
// already does this in main, so the auth module piggybacks on it.
core::Result<Response> perform (const Request& req);

} // namespace gv::auth::http

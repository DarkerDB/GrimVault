#include <curl/curl.h>
#include <gv/api/collection_upload.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace gv::api {

namespace {

std::string lower (std::string value)
{
   std::transform (value.begin (), value.end (), value.begin (),
                   [] (unsigned char ch) { return static_cast<char> (std::tolower (ch)); });
   return value;
}

std::string part (CURLU* url, CURLUPart name)
{
   char* value = nullptr;
   if (curl_url_get (url, name, &value, 0) != CURLUE_OK) return {};
   std::unique_ptr<char, decltype (&curl_free)> owned { value, &curl_free };
   return value;
}

bool token (std::string_view value)
{
   if (value.empty ()) return false;
   return std::all_of (value.begin (), value.end (), [] (unsigned char ch) {
      return std::isalnum (ch) || std::string_view { "!#$%&'*+-.^_`|~" }.contains (ch);
   });
}

}

core::Result<void> validate_collection_upload (std::string_view method, std::string_view value,
                                               const std::vector<core::http::Header>& headers,
                                               const std::vector<std::string>& allowed_hosts)
{
   if (method != "PUT" || value.find_first_of ("\r\n") != std::string_view::npos) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "katforge: unsafe collection upload"));
   }

   std::unique_ptr<CURLU, decltype (&curl_url_cleanup)> url { curl_url (), &curl_url_cleanup };
   const std::string text { value };
   if (!url || curl_url_set (url.get (), CURLUPART_URL, text.c_str (), 0) != CURLUE_OK) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "katforge: unsafe collection upload"));
   }

   const auto scheme = lower (part (url.get (), CURLUPART_SCHEME));
   const auto host = lower (part (url.get (), CURLUPART_HOST));
   const auto port = part (url.get (), CURLUPART_PORT);
   const auto user = part (url.get (), CURLUPART_USER);
   const auto password = part (url.get (), CURLUPART_PASSWORD);
   const bool allowed =
      std::any_of (allowed_hosts.begin (), allowed_hosts.end (),
                   [&host] (const std::string& candidate) { return lower (candidate) == host; });
   if (scheme != "https" || host.empty () || !allowed || (!port.empty () && port != "443") ||
       !user.empty () || !password.empty ()) {
      return core::fail (
         core::Error::make (core::ErrorKind::ExternalApi, "katforge: unsafe collection upload"));
   }

   for (const auto& header : headers) {
      const auto name = lower (header.name);
      if (!token (header.name) || header.value.find_first_of ("\r\n") != std::string::npos ||
          header.value.find ('\0') != std::string::npos ||
          (name != "content-md5" && !name.starts_with ("x-amz-"))) {
         return core::fail (
            core::Error::make (core::ErrorKind::ExternalApi, "katforge: unsafe collection upload"));
      }
   }
   return {};
}

}

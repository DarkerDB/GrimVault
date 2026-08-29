#include <gtest/gtest.h>
#include <gv/auth/oauth_client.h>
#include <gv/core/http.h>

#include <string>

#include "socket_test_support.h"

namespace {

std::string response (int status, std::string body)
{
   return "HTTP/1.1 " + std::to_string (status) +
          " Test\r\n"
          "Content-Type: application/json\r\n"
          "Content-Length: " +
          std::to_string (body.size ()) +
          "\r\n"
          "Connection: close\r\n\r\n" +
          body;
}

gv::auth::OauthClient client (const gv::test::HttpServer& server)
{
   return gv::auth::OauthClient ({
      .client_id = "grimvault",
      .api_base_url = server.url (),
   });
}

}

TEST (OauthClient, MissingTokenDoesNotExposeResponseBody)
{
   gv::core::http::Global curl;
   ASSERT_TRUE (curl);
   gv::test::HttpServer server { response (200, R"({"access_token":"visible-secret"})") };
   ASSERT_NE (server.port (), 0);

   auto oauth = client (server);
   const auto result = oauth.refresh ("refresh-secret");

   ASSERT_FALSE (result.has_value ());
   EXPECT_EQ (result.error ().message.find ("visible-secret"), std::string::npos);
}

TEST (OauthClient, HttpFailureDoesNotExposeResponseBody)
{
   gv::core::http::Global curl;
   ASSERT_TRUE (curl);
   gv::test::HttpServer server { response (401, R"({"error":"server-secret"})") };
   ASSERT_NE (server.port (), 0);

   auto oauth = client (server);
   const auto result = oauth.refresh ("refresh-secret");

   ASSERT_FALSE (result.has_value ());
   EXPECT_EQ (result.error ().message.find ("server-secret"), std::string::npos);
}

#include <gtest/gtest.h>
#include <gv/api/collection_upload.h>

#include <string>
#include <vector>

namespace {

const std::vector<std::string> hosts { "katforge-collections.s3.us-east-2.amazonaws.com" };

bool valid (std::string_view method, std::string_view url,
            std::vector<gv::core::http::Header> headers = {})
{
   return gv::api::validate_collection_upload (method, url, headers, hosts).has_value ();
}

}

TEST (CollectionUpload, AcceptsExpectedPresignedUpload)
{
   EXPECT_TRUE (valid (
      "PUT",
      "https://katforge-collections.s3.us-east-2.amazonaws.com/object?X-Amz-Signature=secret",
      { { "x-amz-checksum-sha256", "value" } }));
}

TEST (CollectionUpload, RejectsUntrustedDestination)
{
   EXPECT_FALSE (valid ("PUT", "http://katforge-collections.s3.us-east-2.amazonaws.com/object"));
   EXPECT_FALSE (valid ("PUT", "https://127.0.0.1/object"));
   EXPECT_FALSE (
      valid ("PUT", "https://katforge-collections.s3.us-east-2.amazonaws.com:8443/object"));
   EXPECT_FALSE (valid ("POST", "https://katforge-collections.s3.us-east-2.amazonaws.com/object"));
}

TEST (CollectionUpload, RejectsUnsafeHeaders)
{
   const std::string url = "https://katforge-collections.s3.us-east-2.amazonaws.com/object";
   EXPECT_FALSE (valid ("PUT", url, { { "Authorization", "secret" } }));
   EXPECT_FALSE (valid ("PUT", url, { { "x-amz-meta-name", "ok\r\nInjected: true" } }));
}

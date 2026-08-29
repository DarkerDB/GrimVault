#include <gtest/gtest.h>
#include <gv/api/darkerdb_client.h>

TEST (ApiResponse, RejectsLookupTypeMismatch)
{
   const auto result = gv::api::parse_lookup_response (R"({"body":{"request_id":[]}})");

   EXPECT_FALSE (result.has_value ());
}

TEST (ApiResponse, RejectsAnalysisTypeMismatch)
{
   const auto result = gv::api::parse_analysis_response (R"({"body":{"match":{"item_id":[]}}})");

   EXPECT_FALSE (result.has_value ());
}

TEST (ApiResponse, RejectsPingTypeMismatch)
{
   const auto result = gv::api::parse_ping_response (R"({"body":{"ok":"yes"}})");

   EXPECT_FALSE (result.has_value ());
}

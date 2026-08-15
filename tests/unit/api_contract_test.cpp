#include <gv/core/api_contract.h>

#include <gtest/gtest.h>

TEST (ApiContractTest, DatedHeaderIsCanonical)
{
   EXPECT_EQ (gv::core::api_contract::header_name, "X-API-Version");
   EXPECT_EQ (gv::core::api_contract::version, "2026-08-15");
   EXPECT_EQ (gv::core::api_contract::header_line, "X-API-Version: 2026-08-15");
}

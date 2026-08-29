#include <gtest/gtest.h>
#include <gv/auth/loopback_server.h>

#include <chrono>
#include <future>
#include <thread>

#include "socket_test_support.h"

TEST (LoopbackServer, CloseInterruptsPartialRequest)
{
   gv::test::SocketRuntime runtime;
   ASSERT_TRUE (runtime.ready ());

   gv::auth::LoopbackServer server;
   const auto port = server.bind ();
   ASSERT_TRUE (port.has_value ()) << port.error ().message;

   auto callback = std::async (std::launch::async, [&server] {
      return server.await_callback ("expected", std::chrono::seconds { 30 });
   });
   const auto client = gv::test::connect_loopback (*port);
   ASSERT_NE (client, test_invalid_socket);
   ASSERT_TRUE (gv::test::send_all (client, "GET /callback?code=partial"));
   std::this_thread::sleep_for (std::chrono::milliseconds { 50 });

   server.close ();

   EXPECT_EQ (callback.wait_for (std::chrono::seconds { 1 }), std::future_status::ready);
   if (callback.wait_for (std::chrono::seconds { 0 }) == std::future_status::ready) {
      EXPECT_FALSE (callback.get ().has_value ());
   }
   gv::test::close_socket (client);
}

#pragma once

#include <gv/core/result.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace gv::auth {

// Outcome of the loopback redirect. Either a code + state on success, or a
// server-supplied error per RFC 6749 §4.1.2.1.
struct CallbackResult {
   std::string code;
   std::string state;
   std::string error;             // empty on success
   std::string error_description; // RFC 6749 §4.1.2.1
};

// Single-shot HTTP-1.1 server bound to 127.0.0.1:<random>. Serves exactly
// one /callback request matching `expected_state`, returns the embedded
// branded close page, then shuts down. Per contract §3.2 step 7.
class LoopbackServer
{
public:
   // Embedded close-tab body. Pulled from the Qt resource compiled into
   // gv_auth; exposed via header so the server can be unit-tested without
   // pulling in Qt.
   static std::string close_page_html ();

   LoopbackServer  ();
   ~LoopbackServer ();

   // Bind to 127.0.0.1:0. On success, returns the kernel-chosen port.
   core::Result<std::uint16_t> bind ();

   // Block until one /callback hit matching `expected_state` lands, or
   // `timeout` elapses, or the server is closed.
   core::Result<CallbackResult> await_callback (
      std::string_view     expected_state,
      std::chrono::seconds timeout
   );

   // Idempotent close. Called automatically from the destructor.
   void close ();

private:
   struct Impl;
   std::unique_ptr<Impl> impl_;
};

} // namespace gv::auth

#include <gv/auth/loopback_server.h>

#include <gv/core/logger.h>

#include <QFile>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
   #include <Winsock2.h>
   #include <Ws2tcpip.h>
   #pragma comment (lib, "Ws2_32.lib")
   using socklen_t = int;
   using sock_t    = SOCKET;
   constexpr sock_t k_invalid_sock = INVALID_SOCKET;
   static int   sock_errno      ()       { return ::WSAGetLastError (); }
   static void  sock_close      (sock_t s) { ::closesocket (s); }
#else
   #include <arpa/inet.h>
   #include <netinet/in.h>
   #include <sys/socket.h>
   #include <sys/types.h>
   #include <unistd.h>
   #include <errno.h>
   using sock_t = int;
   constexpr sock_t k_invalid_sock = -1;
   static int   sock_errno      ()       { return errno; }
   static void  sock_close      (sock_t s) { ::close (s); }
#endif

namespace gv::auth {

namespace {

   struct WsaGuard {
#ifdef _WIN32
      bool ok = false;
      WsaGuard ()
      {
         WSADATA wsa {};
         ok = (::WSAStartup (MAKEWORD (2, 2), &wsa) == 0);
      }
      ~WsaGuard ()
      {
         if (ok) ::WSACleanup ();
      }
#endif
   };

   // Parse "GET /callback?code=...&state=... HTTP/1.1" into a query string.
   std::optional<std::string> parse_query (const std::string& request)
   {
      const auto sp1 = request.find (' ');
      if (sp1 == std::string::npos || request.substr (0, sp1) != "GET") return std::nullopt;
      const auto sp2 = request.find (' ', sp1 + 1);
      if (sp2 == std::string::npos) return std::nullopt;

      const auto path = request.substr (sp1 + 1, sp2 - sp1 - 1);
      const auto qmark = path.find ('?');
      if (qmark == std::string::npos) return std::nullopt;

      // Require /callback path.
      if (path.substr (0, qmark) != "/callback") return std::nullopt;
      return path.substr (qmark + 1);
   }

   std::string url_decode (std::string_view s)
   {
      std::string out;
      out.reserve (s.size ());
      for (std::size_t i = 0; i < s.size (); ++i) {
         const char c = s [i];
         if (c == '%' && i + 2 < s.size ()) {
            const auto hex_val = [] (char h) -> int {
               if (h >= '0' && h <= '9') return h - '0';
               if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
               if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
               return -1;
            };
            const int hi = hex_val (s [i + 1]);
            const int lo = hex_val (s [i + 2]);
            if (hi >= 0 && lo >= 0) {
               out.push_back (static_cast<char> ((hi << 4) | lo));
               i += 2;
               continue;
            }
         }
         if (c == '+') { out.push_back (' '); continue; }
         out.push_back (c);
      }
      return out;
   }

   void parse_callback (std::string_view query, CallbackResult& out)
   {
      while (!query.empty ()) {
         const auto amp = query.find ('&');
         const auto pair = (amp == std::string_view::npos)
            ? query : query.substr (0, amp);
         const auto eq = pair.find ('=');
         if (eq != std::string_view::npos) {
            const auto k = pair.substr (0, eq);
            const auto v = url_decode (pair.substr (eq + 1));
            if      (k == "code")              out.code              = v;
            else if (k == "state")             out.state             = v;
            else if (k == "error")             out.error             = v;
            else if (k == "error_description") out.error_description = v;
         }
         if (amp == std::string_view::npos) break;
         query.remove_prefix (amp + 1);
      }
   }

} // namespace

std::string LoopbackServer::close_page_html ()
{
   QFile f { QStringLiteral (":/auth/oauth-close.html") };
   if (!f.open (QIODevice::ReadOnly)) {
      // Fallback so the redirect never fails outright.
      return "<!doctype html><meta charset=utf-8><title>Signed in</title>"
             "<body><h1>Signed in.</h1><p>You can close this tab.</p></body>";
   }
   const auto data = f.readAll ();
   return std::string { data.constData (), static_cast<std::size_t> (data.size ()) };
}

struct LoopbackServer::Impl
{
   WsaGuard          wsa;
   std::atomic<sock_t> listen_sock { k_invalid_sock };
   std::uint16_t     port        = 0;
   std::atomic<bool> closed { false };

   ~Impl () { close_now (); }

   void close_now ()
   {
      const bool was_open = !closed.exchange (true);
      const sock_t sock = listen_sock.exchange (k_invalid_sock);
      if (was_open && sock != k_invalid_sock) {
#ifdef _WIN32
         ::shutdown (sock, SD_BOTH);
#else
         ::shutdown (sock, SHUT_RDWR);
#endif
         sock_close (sock);
      }
   }
};

LoopbackServer::LoopbackServer  () : impl_ (std::make_unique<Impl> ()) {}
LoopbackServer::~LoopbackServer () = default;

void LoopbackServer::close () { impl_->close_now (); }

core::Result<std::uint16_t> LoopbackServer::bind ()
{
#ifdef _WIN32
   if (!impl_->wsa.ok) {
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "loopback: WSAStartup failed"));
   }
#endif

   const sock_t listen_sock = ::socket (AF_INET, SOCK_STREAM, 0);
   impl_->listen_sock.store (listen_sock);
   if (listen_sock == k_invalid_sock) {
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "loopback: socket() failed: {}", sock_errno ()));
   }

   sockaddr_in addr {};
   addr.sin_family      = AF_INET;
   addr.sin_addr.s_addr = ::htonl (INADDR_LOOPBACK);
   addr.sin_port        = 0;

   if (::bind (listen_sock,
               reinterpret_cast<sockaddr*> (&addr),
               sizeof (addr)) != 0) {
      const int e = sock_errno ();
      impl_->close_now ();
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "loopback: bind() failed: {}", e));
   }

   sockaddr_in bound {};
   socklen_t bound_len = sizeof (bound);
   if (::getsockname (listen_sock,
                      reinterpret_cast<sockaddr*> (&bound),
                      &bound_len) != 0) {
      const int e = sock_errno ();
      impl_->close_now ();
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "loopback: getsockname() failed: {}", e));
   }
   impl_->port = ::ntohs (bound.sin_port);

   if (::listen (listen_sock, 1) != 0) {
      const int e = sock_errno ();
      impl_->close_now ();
      return core::fail (core::Error::make (core::ErrorKind::Io,
         "loopback: listen() failed: {}", e));
   }

   core::log::api.info ("loopback: listening on 127.0.0.1:{}", impl_->port);
   return impl_->port;
}

core::Result<CallbackResult> LoopbackServer::await_callback (
   std::string_view     expected_state,
   std::chrono::seconds timeout,
   std::string_view     success_redirect,
   std::string_view     error_redirect
) {
   if (impl_->listen_sock.load () == k_invalid_sock) {
      return core::fail (core::Error::make (core::ErrorKind::Internal,
         "loopback: server not bound"));
   }

   const auto deadline = std::chrono::steady_clock::now () + timeout;

   for (;;) {
      const sock_t listen_sock = impl_->listen_sock.load ();
      if (listen_sock == k_invalid_sock || impl_->closed.load ()) {
         return core::fail (core::Error::make (core::ErrorKind::Io,
            "Sign-in was cancelled."));
      }
      // Use select() so we can honor the deadline without blocking forever.
      const auto remaining = std::chrono::duration_cast<std::chrono::seconds> (
         deadline - std::chrono::steady_clock::now ());
      if (remaining.count () <= 0) {
         return core::fail (core::Error::make (core::ErrorKind::Io,
            "Sign-in timed out waiting for the browser callback."));
      }

      fd_set rfds;
      FD_ZERO (&rfds);
      FD_SET (listen_sock, &rfds);
      timeval tv { static_cast<long> (remaining.count ()), 0 };

      const int sel = ::select (static_cast<int> (listen_sock + 1),
                                &rfds, nullptr, nullptr, &tv);
      if (sel < 0) {
         if (impl_->closed.load ()) {
            return core::fail (core::Error::make (core::ErrorKind::Io,
               "Sign-in was cancelled."));
         }
         return core::fail (core::Error::make (core::ErrorKind::Io,
            "loopback: select failed: {}", sock_errno ()));
      }
      if (sel == 0) continue;

      sockaddr_in peer {};
      socklen_t peer_len = sizeof (peer);
      sock_t client = ::accept (listen_sock,
                                reinterpret_cast<sockaddr*> (&peer),
                                &peer_len);
      if (client == k_invalid_sock) continue;

      // Read up to 8 KB of the request — only the request line matters.
      std::string buf;
      buf.reserve (4096);
      char chunk [2048];
      for (;;) {
         const auto n = ::recv (client, chunk, sizeof (chunk), 0);
         if (n <= 0) break;
         buf.append (chunk, static_cast<std::size_t> (n));
         if (buf.find ("\r\n\r\n") != std::string::npos) break;
         if (buf.size () > 16384) break;
      }

      const auto query = parse_query (buf);
      if (!query.has_value ()) {
         constexpr std::string_view response =
            "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
         ::send (client, response.data (), static_cast<int> (response.size ()), 0);
         sock_close (client);
         continue;
      }
      CallbackResult cb;
      parse_callback (*query, cb);

      const bool state_matches = cb.state == std::string { expected_state };
      const bool terminal_error = state_matches && !cb.error.empty ();
      const bool is_success = state_matches && cb.error.empty () && !cb.code.empty ();

      if (!terminal_error && !is_success) {
         constexpr std::string_view response =
            "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n"
            "Cache-Control: no-store\r\n\r\n";
         ::send (client, response.data (), static_cast<int> (response.size ()), 0);
         sock_close (client);
         continue;
      }

      // Prefer a 302 redirect to the realm SPA so the success/failure
      // page lives in the brand. Fall back to the embedded HTML when
      // no redirect URL was provided (CLI/test contexts).
      const std::string redirect_url = is_success
         ? std::string { success_redirect }
         : std::string { error_redirect };

      std::string response;
      if (!redirect_url.empty ()) {
         response =
            std::string { is_success ? "HTTP/1.1 302 Found\r\n" : "HTTP/1.1 302 Found\r\n" } +
            "Location: " + redirect_url + "\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-store\r\n"
            "\r\n";
      } else {
         const std::string body = close_page_html ();
         const std::string status_line = is_success
            ? "HTTP/1.1 200 OK\r\n"
            : "HTTP/1.1 400 Bad Request\r\n";

         response =
            status_line +
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: " + std::to_string (body.size ()) + "\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-store\r\n"
            "\r\n" + body;
      }

      ::send (client, response.data (), static_cast<int> (response.size ()), 0);
      sock_close (client);

      if (!cb.error.empty ()) {
         const std::string msg = cb.error_description.empty ()
            ? "Authorization error: " + cb.error
            : "Authorization error: " + cb.error_description + " (" + cb.error + ")";
         return core::fail (core::Error::make (
            core::ErrorKind::ExternalApi, "{}", msg));
      }
      return cb;
   }
}

} // namespace gv::auth

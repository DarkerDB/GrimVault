#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <Winsock2.h>
#include <Ws2tcpip.h>
using test_socket_t = SOCKET;
using test_socklen_t = int;
inline constexpr test_socket_t test_invalid_socket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using test_socket_t = int;
using test_socklen_t = socklen_t;
inline constexpr test_socket_t test_invalid_socket = -1;
#endif

namespace gv::test {

inline void close_socket (test_socket_t socket)
{
   if (socket == test_invalid_socket) return;
#ifdef _WIN32
   ::shutdown (socket, SD_BOTH);
   ::closesocket (socket);
#else
   ::shutdown (socket, SHUT_RDWR);
   ::close (socket);
#endif
}

class SocketRuntime
{
  public:
   SocketRuntime ()
   {
#ifdef _WIN32
      WSADATA data {};
      ready_ = ::WSAStartup (MAKEWORD (2, 2), &data) == 0;
#endif
   }

   ~SocketRuntime ()
   {
#ifdef _WIN32
      if (ready_) ::WSACleanup ();
#endif
   }

   bool ready () const { return ready_; }

  private:
   bool ready_ = true;
};

inline test_socket_t connect_loopback (std::uint16_t port)
{
   const auto socket = ::socket (AF_INET, SOCK_STREAM, 0);
   if (socket == test_invalid_socket) return socket;

   sockaddr_in address {};
   address.sin_family = AF_INET;
   address.sin_addr.s_addr = ::htonl (INADDR_LOOPBACK);
   address.sin_port = ::htons (port);
   if (::connect (socket, reinterpret_cast<sockaddr*> (&address), sizeof (address)) != 0) {
      close_socket (socket);
      return test_invalid_socket;
   }
   return socket;
}

inline bool send_all (test_socket_t socket, std::string_view value)
{
   while (!value.empty ()) {
      const auto sent = ::send (socket, value.data (), static_cast<int> (value.size ()), 0);
      if (sent <= 0) return false;
      value.remove_prefix (static_cast<std::size_t> (sent));
   }
   return true;
}

class HttpServer
{
  public:
   explicit HttpServer (std::string response) : response_ (std::move (response))
   {
      if (!runtime_.ready ()) return;
      const auto listener = ::socket (AF_INET, SOCK_STREAM, 0);
      listener_.store (listener);
      if (listener == test_invalid_socket) return;

      sockaddr_in address {};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = ::htonl (INADDR_LOOPBACK);
      if (::bind (listener, reinterpret_cast<sockaddr*> (&address), sizeof (address)) != 0 ||
          ::listen (listener, 1) != 0) {
         close (listener_);
         return;
      }

      test_socklen_t size = sizeof (address);
      if (::getsockname (listener, reinterpret_cast<sockaddr*> (&address), &size) != 0) {
         close (listener_);
         return;
      }
      port_ = ::ntohs (address.sin_port);
      worker_ = std::thread ([this, listener] { serve (listener); });
   }

   ~HttpServer ()
   {
      close (client_);
      close (listener_);
      if (worker_.joinable ()) worker_.join ();
   }

   std::uint16_t port () const { return port_; }
   std::string url () const { return "http://127.0.0.1:" + std::to_string (port_); }

  private:
   static void close (std::atomic<test_socket_t>& slot)
   {
      close_socket (slot.exchange (test_invalid_socket));
   }

   void serve (test_socket_t listener)
   {
      sockaddr_in address {};
      test_socklen_t size = sizeof (address);
      const auto client = ::accept (listener, reinterpret_cast<sockaddr*> (&address), &size);
      client_.store (client);
      if (client == test_invalid_socket) return;

      std::string request;
      char chunk[2048];
      while (request.find ("\r\n\r\n") == std::string::npos) {
         const auto received = ::recv (client, chunk, sizeof (chunk), 0);
         if (received <= 0) return;
         request.append (chunk, static_cast<std::size_t> (received));
      }
      send_all (client, response_);
      close (client_);
   }

   SocketRuntime runtime_;
   std::atomic<test_socket_t> listener_ { test_invalid_socket };
   std::atomic<test_socket_t> client_ { test_invalid_socket };
   std::uint16_t port_ = 0;
   std::string response_;
   std::thread worker_;
};

}

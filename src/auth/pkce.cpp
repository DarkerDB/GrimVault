#include <gv/auth/pkce.h>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

namespace gv::auth {

namespace {

   constexpr char k_b64url [] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

   std::string random_b64url (std::size_t bytes)
   {
      std::string buf (bytes, '\0');
      if (RAND_bytes (reinterpret_cast<unsigned char*> (buf.data ()),
                      static_cast<int> (bytes)) != 1) {
         throw std::runtime_error { "pkce: RAND_bytes failed" };
      }
      return base64url_encode (
         reinterpret_cast<const unsigned char*> (buf.data ()), bytes);
   }

} // namespace

std::string base64url_encode (const unsigned char* data, std::size_t len)
{
   std::string out;
   out.reserve ((len * 4 + 2) / 3);

   std::size_t i = 0;
   for (; i + 3 <= len; i += 3) {
      const unsigned a = data [i];
      const unsigned b = data [i + 1];
      const unsigned c = data [i + 2];
      out.push_back (k_b64url [(a >> 2) & 0x3F]);
      out.push_back (k_b64url [((a << 4) | (b >> 4)) & 0x3F]);
      out.push_back (k_b64url [((b << 2) | (c >> 6)) & 0x3F]);
      out.push_back (k_b64url [c & 0x3F]);
   }

   const std::size_t rem = len - i;
   if (rem == 1) {
      const unsigned a = data [i];
      out.push_back (k_b64url [(a >> 2) & 0x3F]);
      out.push_back (k_b64url [(a << 4) & 0x3F]);
   } else if (rem == 2) {
      const unsigned a = data [i];
      const unsigned b = data [i + 1];
      out.push_back (k_b64url [(a >> 2) & 0x3F]);
      out.push_back (k_b64url [((a << 4) | (b >> 4)) & 0x3F]);
      out.push_back (k_b64url [(b << 2) & 0x3F]);
   }

   return out;
}

PkcePair pkce_generate ()
{
   PkcePair out;
   out.verifier = random_b64url (64);

   std::array<unsigned char, SHA256_DIGEST_LENGTH> digest {};
   SHA256 (reinterpret_cast<const unsigned char*> (out.verifier.data ()),
           out.verifier.size (), digest.data ());

   out.challenge = base64url_encode (digest.data (), digest.size ());
   return out;
}

std::string state_generate ()
{
   return random_b64url (32);
}

} // namespace gv::auth

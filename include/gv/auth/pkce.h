#pragma once

#include <string>

namespace gv::auth {

// PKCE primitives per RFC 7636. Verifier is 64 bytes of CSPRNG output
// rendered base64url-no-padding (86 chars). Challenge is base64url-no-padding
// of SHA-256 (verifier).
struct PkcePair {
   std::string verifier;
   std::string challenge;
};

PkcePair pkce_generate ();

// 32-byte CSPRNG string, base64url-no-padding (43 chars). Used as the OAuth
// `state` value.
std::string state_generate ();

// base64url-no-padding encoder. Exposed for tests and because the OAuth /
// loopback path needs it in a couple of other places.
std::string base64url_encode (const unsigned char* data, std::size_t len);

} // namespace gv::auth

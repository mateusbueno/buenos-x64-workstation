#pragma once
// Story 6.8: small helpers shared by the registry (clock, hex, key files, JSON output). Vendor-side only; see tools/registry/README.md.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "crypto.hpp"

namespace velora::registry {

using velora::contentpack::Bytes;
using velora::contentpack::Key32;

std::int64_t now_ms();                                  // wall clock, Unix milliseconds
std::string rfc3339(std::int64_t unix_ms);              // "2026-10-20T12:34:56Z"
std::string hex_encode(const std::uint8_t* p, std::size_t n);
bool hex_decode32(std::string_view text, Key32& out);   // exactly 64 hex digits, either case
bool hex_decode_exact(std::string_view text, std::uint8_t* out, std::size_t n);
std::string json_escape(std::string_view s);            // the content of a JSON string literal, without the quotes
std::string json_str(std::string_view s);               // "..." with escapes

// Raw key file: a regular file of exactly `n` bytes, not readable by group or others (secret files), read into `out` (which the caller keeps in a Secret).
// On failure `err` says why (never the content).
bool read_secret_file(const std::string& path, std::uint8_t* out, std::size_t n, std::string* err);
// PEM / config file that must not be group- or world-accessible (the server's TLS private key).
bool check_private_mode(const std::string& path, std::string* err);

// Token charsets of the data model: [A-Za-z0-9][A-Za-z0-9._:/-]{0,max-1}.
bool valid_token(std::string_view s, std::size_t max_len);
// device_id / replaces: [a-z0-9][a-z0-9._:-]{0,max-1}. No '/' (it is a path segment), no upper case (ids are compared byte for byte, so case variants would be distinct ids
// that look alike); serial numbers are compared case-insensitively instead.
bool valid_id_token(std::string_view s, std::size_t max_len);
// X25519 public key sanity: bit 255 clear, value < p = 2^255-19 (canonical), and not one of the known small-order points (0, 1, the two order-8 points, p-1).
// Proof of possession of the private key is NOT checked (documented future item): the registry can not tell a key the unit holds from any other good point.
bool acceptable_x25519_public(const Key32& k, std::string* why);
bool valid_date(std::string_view s);                    // YYYY-MM-DD, a real calendar day
bool valid_library_id(std::string_view s);              // canonical lower-case 8-4-4-4-12 UUID text

}  // namespace velora::registry

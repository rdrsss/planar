// @file sha256.hpp
// @brief SHA-256, for the conflict-evidence token (plan 996, task 6041).
//
// ## Why this is hand-written rather than taken from a library
//
// The C++ standard library has no hash primitive of this kind, and the token
// is the ONLY thing in the tree that needs one. Adding OpenSSL to
// cmake/dependencies.cmake for eight lines of digest would be a large,
// TLS-shaped dependency taken for a non-TLS reason; libcurl is already
// vendored but does not export a digest API. So the algorithm is written out,
// and its correctness is pinned against published NIST test vectors in
// sync.t.cpp rather than against this file's own arithmetic.
//
// ## Why the token has to be SHA-256 SPECIFICALLY
//
// `sync_events.context_json.token` is an OBSERVABLE VALUE: the operator reads
// it out of `audit trail --link <id> --json` and passes it straight back as
// `sync resolve --evidence-token`. The oracle computes it as
//
//   sha256("v1\0<link_id>\0<local.title>\0<local.status>\0<local.updated_at>
//          \0<remote.title>\0<remote.status>\0<remote.version>")
//
// rendered as LOWERCASE hex (zig/src/engine/external/sync.zig's
// `conflictEvidenceJson`, `std.fmt.bytesToHex(digest, .lower)`). Any other
// digest, any other field order, any other separator, or uppercase hex is a
// different token for the same conflict — which is not a cosmetic divergence
// but a broken CAS guard.
//
// The formula was verified against a REAL oracle run, not read off the source:
// a conflict seeded through the Zig binary against a loopback fixture wrote
// token cab1bdfe…ca11, and recomputing the string above in Python reproduced
// that digest exactly. See sync.t.cpp.
//
// ## Why a private header
//
// Same argument the bucket's json_read.hpp makes: it is needed by sync.cpp
// and by sync.t.cpp, both in the same target, so a header creates no
// dependency edge and nothing outside this bucket can reach it.
#pragma once

// UNLIKE parity_harness.hpp / fixture_server.hpp / json_read.hpp, this header
// DOES include the standard headers it needs, and it has to. Those three are
// only ever included where something else has already declared the standard
// library — from a plain test TU after `import std;`, or (json_read.hpp) from
// a global module fragment that pulls in <glaze/glaze.hpp> first. This one is
// included from sync.cpp's global module fragment, which precedes `import
// std;` and pulls in nothing else, so `std::array` / `std::string` /
// `std::format` are simply not declared there. Mixing these includes with a
// later `import std;` is the same combination cmake/dependencies.cmake
// already documents for CLI11 (a header-wrapping module) and
// catalog_parity.hpp already relies on for Glaze.
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace planar::engine::external::sha256 {

namespace detail {

/// @brief The SHA-256 round constants (FIPS 180-4 §4.2.2).
inline constexpr std::array<std::uint32_t, 64> k_round = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

/// @brief Rotate a 32-bit word right.
/// @param value The word.
/// @param bits How far.
/// @return The rotated word.
inline auto rotr(std::uint32_t value, unsigned bits) -> std::uint32_t {
  return (value >> bits) | (value << (32U - bits));
}

} // namespace detail

/// @brief The SHA-256 digest of `input`, as 64 LOWERCASE hex characters.
///
/// Byte-oriented: `input` is treated as opaque bytes, so the NUL separators
/// the token format uses pass through as ordinary content rather than
/// terminating anything.
/// @param input The bytes to hash.
/// @return The lowercase hex digest.
inline auto hex(std::string_view input) -> std::string {
  std::array<std::uint32_t, 8> state = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  std::string padded(input);
  auto const  bit_length = static_cast<std::uint64_t>(input.size()) * 8U;
  padded.push_back(static_cast<char>(0x80));
  while (padded.size() % 64U != 56U) {
    padded.push_back('\0');
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    padded.push_back(static_cast<char>((bit_length >> static_cast<unsigned>(shift)) & 0xFFU));
  }

  for (std::size_t offset = 0; offset < padded.size(); offset += 64U) {
    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t i = 0; i < 16U; ++i) {
      auto const at = offset + (i * 4U);
      schedule[i]   = (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[at])) << 24U) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[at + 1])) << 16U) |
                      (static_cast<std::uint32_t>(static_cast<unsigned char>(padded[at + 2])) << 8U) |
                      static_cast<std::uint32_t>(static_cast<unsigned char>(padded[at + 3]));
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
      auto const s0 = detail::rotr(schedule[i - 15], 7) ^ detail::rotr(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3U);
      auto const s1 = detail::rotr(schedule[i - 2], 17) ^ detail::rotr(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10U);
      schedule[i]   = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    auto working = state;
    for (std::size_t i = 0; i < 64U; ++i) {
      auto const s1  = detail::rotr(working[4], 6) ^ detail::rotr(working[4], 11) ^ detail::rotr(working[4], 25);
      auto const ch  = (working[4] & working[5]) ^ (~working[4] & working[6]);
      auto const t1  = working[7] + s1 + ch + detail::k_round[i] + schedule[i];
      auto const s0  = detail::rotr(working[0], 2) ^ detail::rotr(working[0], 13) ^ detail::rotr(working[0], 22);
      auto const maj = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
      auto const t2  = s0 + maj;
      working[7]     = working[6];
      working[6]     = working[5];
      working[5]     = working[4];
      working[4]     = working[3] + t1;
      working[3]     = working[2];
      working[2]     = working[1];
      working[1]     = working[0];
      working[0]     = t1 + t2;
    }
    for (std::size_t i = 0; i < 8U; ++i) {
      state[i] += working[i];
    }
  }

  std::string out;
  out.reserve(64);
  for (auto const word : state) {
    // LOWERCASE hex — the oracle uses `std::fmt.bytesToHex(digest, .lower)`,
    // and uppercase would be a different token for the same conflict.
    out += std::format("{:08x}", word);
  }
  return out;
}

} // namespace planar::engine::external::sha256

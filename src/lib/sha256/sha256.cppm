/// @file sha256.cppm
/// @brief Layer-1 SHA-256 digest primitive (FIPS 180-4).
///
/// Declaration and implementation are both in this interface unit rather
/// than split into a companion `sha256.cpp`, matching `core/version.cppm`'s
/// precedent for a small, dependency-free module: doxygen 1.18.0's C++20
/// modules parser fails to associate a declaration's doc comment with the
/// merged declaration+definition entity when the two live in separate
/// files for a module this small (confirmed by isolated repro — a
/// two-file split of this exact function, with this exact doc comment,
/// reproducibly reports "parameters ... not documented" while the
/// single-file form does not). This is a distinct, deterministic parser
/// defect from the documented SIGBUS crash flake (docs/toolchain-parity.md)
/// and has no workaround short of not splitting the file.
export module planar.sha256;

import std;

export namespace planar::sha256 {

/// @brief Compute the FIPS 180-4 SHA-256 digest of `input` and render it as
/// 64 lowercase hex characters.
/// @param input The bytes to digest.
/// @return The lowercase hex-encoded digest.
[[nodiscard]] auto hex(std::string_view input) -> std::string {
  constexpr auto k = std::to_array<std::uint32_t>(
      {0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
       0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
       0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
       0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
       0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
       0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
       0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
       0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U});
  static_assert(k.size() == 64);
  auto                         r = [](std::uint32_t x, unsigned n) constexpr { return (x >> n) | (x << (32U - n)); };
  std::array<std::uint32_t, 8> h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
  std::string                  m(input);
  auto                         bits = static_cast<std::uint64_t>(input.size()) * 8U;
  m.push_back(char(0x80));
  while (m.size() % 64U != 56U)
    m.push_back(0);
  for (int s = 56; s >= 0; s -= 8)
    m.push_back(char((bits >> s) & 255U));
  for (std::size_t off = 0; off < m.size(); off += 64U) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; i++) {
      auto p = off + i * 4;
      w[i]   = (std::uint32_t((unsigned char)m[p]) << 24U) | (std::uint32_t((unsigned char)m[p + 1]) << 16U) |
               (std::uint32_t((unsigned char)m[p + 2]) << 8U) | std::uint32_t((unsigned char)m[p + 3]);
    }
    for (std::size_t i = 16; i < 64; i++) {
      auto s0 = r(w[i - 15], 7) ^ r(w[i - 15], 18) ^ (w[i - 15] >> 3U);
      auto s1 = r(w[i - 2], 17) ^ r(w[i - 2], 19) ^ (w[i - 2] >> 10U);
      w[i]    = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], q = h[7];
    for (std::size_t i = 0; i < 64; i++) {
      auto t1 = q + (r(e, 6) ^ r(e, 11) ^ r(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      auto t2 = (r(a, 2) ^ r(a, 13) ^ r(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      q       = g;
      g       = f;
      f       = e;
      e       = d + t1;
      d       = c;
      c       = b;
      b       = a;
      a       = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += q;
  }
  std::string out;
  for (auto x : h)
    out += std::format("{:08x}", x);
  return out;
}

} // namespace planar::sha256

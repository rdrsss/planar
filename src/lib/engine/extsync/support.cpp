/// @file support.cpp
/// @brief Implementation of `planar.engine.extsync.support`.

module planar.engine.extsync.support;

import std;
import planar.adapter;

namespace planar::engine::extsync::support {

namespace {

/// @brief Standard-alphabet, padded base64 — Zig's `std.base64.standard`.
/// @param raw The bytes to encode.
/// @return The encoded text.
auto base64_standard(std::string_view raw) -> std::string {
  constexpr std::string_view k_alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string                out;
  out.reserve(((raw.size() + 2) / 3) * 4);
  std::size_t i = 0;
  while (i + 3 <= raw.size()) {
    auto const block = (static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i])) << 16U) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i + 1])) << 8U) |
                       static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i + 2]));
    out.push_back(k_alphabet[(block >> 18U) & 0x3FU]);
    out.push_back(k_alphabet[(block >> 12U) & 0x3FU]);
    out.push_back(k_alphabet[(block >> 6U) & 0x3FU]);
    out.push_back(k_alphabet[block & 0x3FU]);
    i += 3;
  }
  auto const rest = raw.size() - i;
  if (rest == 1) {
    auto const block = static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i])) << 16U;
    out.push_back(k_alphabet[(block >> 18U) & 0x3FU]);
    out.push_back(k_alphabet[(block >> 12U) & 0x3FU]);
    out += "==";
  } else if (rest == 2) {
    auto const block = (static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i])) << 16U) |
                       (static_cast<std::uint32_t>(static_cast<unsigned char>(raw[i + 1])) << 8U);
    out.push_back(k_alphabet[(block >> 18U) & 0x3FU]);
    out.push_back(k_alphabet[(block >> 12U) & 0x3FU]);
    out.push_back(k_alphabet[(block >> 6U) & 0x3FU]);
    out.push_back('=');
  }
  return out;
}

} // namespace

auto auth_header(const adapter::auth_credential& cred) -> std::expected<std::string, adapter::adapter_error> {
  switch (cred.kind) {
  case adapter::auth_kind::bearer: {
    if (!cred.token.has_value()) {
      return std::unexpected(adapter::adapter_error::invalid_auth);
    }
    return std::format("Bearer {}", *cred.token);
  }
  case adapter::auth_kind::basic: {
    if (!cred.user.has_value() || !cred.pass.has_value()) {
      return std::unexpected(adapter::adapter_error::invalid_auth);
    }
    return std::format("Basic {}", base64_standard(std::format("{}:{}", *cred.user, *cred.pass)));
  }
  }
  return std::unexpected(adapter::adapter_error::invalid_auth);
}

auto trim_trailing_slash(std::string_view s) -> std::string_view {
  if (!s.empty() && s.back() == '/') {
    return s.substr(0, s.size() - 1);
  }
  return s;
}

} // namespace planar::engine::extsync::support

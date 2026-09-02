/// @file sha256.cppm
/// @brief Layer-1 SHA-256 digest primitive (FIPS 180-4).
export module planar.sha256;

import std;

export namespace planar::sha256 {

/// @brief Compute the FIPS 180-4 SHA-256 digest of `input` and render it as
/// 64 lowercase hex characters.
/// @param input The bytes to digest.
/// @return The lowercase hex-encoded digest.
export [[nodiscard]] auto hex(std::string_view input) -> std::string;

} // namespace planar::sha256

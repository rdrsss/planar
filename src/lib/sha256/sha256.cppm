/// @brief Layer-1 SHA-256 digest primitive (FIPS 180-4).
export module planar.sha256;
import std;
export namespace planar::sha256 {
[[nodiscard]] auto hex(std::string_view input) -> std::string;
}

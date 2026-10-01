// sql_scan.hpp: the C++ string-literal scanner the hostqueue source-reading
// tests share (plan 1089, tasks qp-queue-compat and qp-separability; tech spec
// 656 § Enforcement and § Separability).
//
// Two tests read first-party sources and judge only their SQL string
// literals: the protocol fingerprint (schema.t.cpp) and the separability
// guard (separability.t.cpp). Both must split a source into literals the same
// way, so the scanner lives here once. A header, included rather than
// linked, like `scratch_store.hpp`: the includer includes Catch2 and imports
// `std` first, and `REQUIRE` below needs Catch2's macros.
#pragma once

namespace hostqueue_scan {

/// @brief Whether `c` can be part of a C++ identifier.
/// @param c The character.
/// @return True for a letter, a digit or an underscore.
inline auto is_ident_char(char c) -> bool {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Every string literal in C++ source `src`, in source order, with adjacent
// literals (separated only by whitespace or comments) concatenated as the
// compiler does. Comments, character literals and digit separators are
// skipped; raw strings are read verbatim; escapes are kept as written.
inline auto string_literals(std::string_view src) -> std::vector<std::string> {
  std::vector<std::string> out;
  bool                     adjacent = false;
  std::size_t              i        = 0;
  while (i < src.size()) {
    if (src.substr(i, 2) == "//") {
      i = src.find('\n', i);
      if (i == std::string_view::npos) {
        break;
      }
      continue;
    }
    if (src.substr(i, 2) == "/*") {
      auto const end = src.find("*/", i + 2);
      i              = end == std::string_view::npos ? src.size() : end + 2;
      continue;
    }
    char const c = src[i];
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      ++i;
      continue;
    }
    if (c == '"') {
      std::string body;
      if (i > 0 && src[i - 1] == 'R') {
        auto const open  = src.find('(', i);
        auto const delim = src.substr(i + 1, open - i - 1);
        auto const close = src.find(std::format("){}\"", delim), open);
        REQUIRE(close != std::string_view::npos);
        body = std::string(src.substr(open + 1, close - open - 1));
        i    = close + delim.size() + 2;
      } else {
        std::size_t j = i + 1;
        while (j < src.size() && src[j] != '"') {
          auto const width = (src[j] == '\\' && j + 1 < src.size()) ? 2U : 1U;
          body += src.substr(j, width);
          j += width;
        }
        i = j + 1;
      }
      if (adjacent) {
        out.back() += body;
      } else {
        out.push_back(std::move(body));
      }
      adjacent = true;
      continue;
    }
    if (c == '\'') {
      if (i > 0 && std::isalnum(static_cast<unsigned char>(src[i - 1])) != 0) {
        ++i; // a digit separator: 86'400'000
        continue;
      }
      std::size_t j = i + 1;
      while (j < src.size() && src[j] != '\'') {
        j += (src[j] == '\\') ? 2 : 1;
      }
      i        = j + 1;
      adjacent = false;
      continue;
    }
    if (is_ident_char(c)) {
      auto j = i;
      while (j < src.size() && is_ident_char(src[j])) {
        ++j;
      }
      // A literal's encoding or raw prefix does not break adjacency.
      auto const word = src.substr(i, j - i);
      if (!(j < src.size() && src[j] == '"' && (word == "R" || word == "u8" || word == "u8R" || word == "L" || word == "LR"))) {
        adjacent = false;
      }
      i = j;
      continue;
    }
    adjacent = false;
    ++i;
  }
  return out;
}

} // namespace hostqueue_scan

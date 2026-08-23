/// @file json_text.cpp
/// @brief Implementation of `planar.json_text` (plan 996, task 6109). See
/// json_text.cppm for why this is a layer-1 module and for the escape table's
/// provenance.

module planar.json_text;

import std;

namespace planar::json_text {

auto append_json_string(std::string& out, std::string_view text) -> void {
  out.push_back('"');
  for (const char raw : text) {
    const auto c = static_cast<unsigned char>(raw);
    switch (c) {
    case '\\':
      out.append("\\\\");
      break;
    case '"':
      out.append("\\\"");
      break;
    case 0x08:
      out.append("\\b");
      break;
    case 0x0C:
      out.append("\\f");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      if (c < 0x20) {
        // LOWERCASE hex, matching std.json — an uppercase `` would still
        // be valid JSON and still be a parity break.
        out.append(std::format("\\u{:04x}", static_cast<unsigned>(c)));
      } else {
        // No escaping of `/`, and non-ASCII bytes pass through as raw UTF-8.
        out.push_back(raw);
      }
      break;
    }
  }
  out.push_back('"');
}

auto json_string(std::string_view text) -> std::string {
  std::string out;
  append_json_string(out, text);
  return out;
}

} // namespace planar::json_text

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
        // LOWERCASE hex, matching std.json -- an uppercase `\u000B` would
        // still be valid JSON and still be a parity break.
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

auto format_double_fixed(double value) -> std::string {
  // Non-finite first: Zig's `{d}` spellings. See the declaration for why
  // these are NOT JSON-safe and why that is deliberate.
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value < 0 ? "-inf" : "inf";
  }

  std::array<char, 64> buffer{};
  auto const [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (ec != std::errc{}) {
    return "0";
  }
  std::string_view text{buffer.data(), ptr};

  auto const exponent_at = text.find('e');
  if (exponent_at == std::string_view::npos) {
    return std::string{text}; // Already plain: "0.1", "-0", "100".
  }

  std::string_view mantissa      = text.substr(0, exponent_at);
  std::string_view exponent_text = text.substr(exponent_at + 1);
  // `to_chars` writes `e+300`, and `std::from_chars` REJECTS a leading `+`
  // outright rather than skipping it -- it fails and leaves the output
  // untouched, so ignoring the failure silently yields exponent 0 and
  // renders `1e300` as `1`. Strip the sign by hand.
  if (!exponent_text.empty() && exponent_text.front() == '+') {
    exponent_text.remove_prefix(1);
  }
  int exponent = 0;
  if (std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent).ec != std::errc{}) {
    return std::string{text};
  }

  std::string sign;
  if (!mantissa.empty() && (mantissa.front() == '-' || mantissa.front() == '+')) {
    if (mantissa.front() == '-') {
      sign = "-";
    }
    mantissa.remove_prefix(1);
  }

  // Collapse the mantissa to bare digits; the point's position is fully
  // recoverable from the exponent, so where it SAT needs no remembering.
  auto const  dot = mantissa.find('.');
  std::string digits{mantissa.substr(0, dot)};
  if (dot != std::string_view::npos) {
    digits += mantissa.substr(dot + 1);
  }
  auto const integral_digits = static_cast<int>(dot == std::string_view::npos ? mantissa.size() : dot);

  // `point` is how many digits belong to the LEFT of the decimal point once
  // the exponent is applied.
  int const point = integral_digits + exponent;

  std::string out{sign};
  if (point <= 0) {
    out += "0.";
    out.append(static_cast<std::size_t>(-point), '0');
    out += digits;
  } else if (static_cast<std::size_t>(point) >= digits.size()) {
    out += digits;
    out.append(static_cast<std::size_t>(point) - digits.size(), '0');
  } else {
    out += digits.substr(0, static_cast<std::size_t>(point));
    out += '.';
    out += digits.substr(static_cast<std::size_t>(point));
  }
  return out;
}

auto append_json_double(std::string& out, double value) -> void {
  if (!std::isfinite(value)) {
    out.append("null");
    return;
  }
  out.append(format_double_fixed(value));
}

auto json_double(double value) -> std::string {
  std::string out;
  append_json_double(out, value);
  return out;
}

} // namespace planar::json_text

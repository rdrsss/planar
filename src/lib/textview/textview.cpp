/// @file textview.cpp
/// @brief Implementation of `planar.textview`. See textview.cppm for the
/// contract.

module planar.textview;

import std;

namespace planar::textview {

namespace {

/// @brief One decoded code point and the bytes it took.
struct decoded {
  char32_t    value  = 0; ///< The code point; the byte itself for an invalid sequence.
  std::size_t length = 1; ///< Bytes consumed, at least 1.
  bool        valid  = true;
};

/// @brief Decode the UTF-8 sequence at `at`. An invalid, truncated, overlong
/// or surrogate sequence yields its first byte alone, marked invalid.
auto decode_at(std::string_view text, std::size_t at) -> decoded {
  auto const lead = static_cast<unsigned char>(text[at]);
  if (lead < 0x80) {
    return {.value = lead, .length = 1, .valid = true};
  }
  std::size_t extra = 0;
  char32_t    cp    = 0;
  char32_t    least = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    extra = 1;
    cp    = lead & 0x1Fu;
    least = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    extra = 2;
    cp    = lead & 0x0Fu;
    least = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    extra = 3;
    cp    = lead & 0x07u;
    least = 0x10000;
  } else {
    return {.value = lead, .length = 1, .valid = false};
  }
  if (at + extra >= text.size()) {
    return {.value = lead, .length = 1, .valid = false};
  }
  for (std::size_t i = 1; i <= extra; ++i) {
    auto const next = static_cast<unsigned char>(text[at + i]);
    if ((next & 0xC0u) != 0x80u) {
      return {.value = lead, .length = 1, .valid = false};
    }
    cp = (cp << 6) | (next & 0x3Fu);
  }
  if (cp < least || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    return {.value = lead, .length = 1, .valid = false};
  }
  return {.value = cp, .length = extra + 1, .valid = true};
}

auto in_range(char32_t cp, char32_t lo, char32_t hi) -> bool {
  return cp >= lo && cp <= hi;
}

/// @brief Whether a code point is a C1 control (U+0080-U+009F).
auto is_c1(char32_t cp) -> bool {
  return in_range(cp, 0x80, 0x9F);
}

/// @brief Columns one valid code point takes: 0 for a combining mark or a
/// format character, 2 for a wide East Asian or emoji code point, else 1.
auto cell_width(char32_t cp) -> std::size_t {
  if (is_format_char(cp) || in_range(cp, 0x0300, 0x036F) || in_range(cp, 0x1AB0, 0x1AFF) || in_range(cp, 0x1DC0, 0x1DFF) ||
      in_range(cp, 0x20D0, 0x20FF) || in_range(cp, 0xFE00, 0xFE0F) || in_range(cp, 0xFE20, 0xFE2F)) {
    return 0;
  }
  if (in_range(cp, 0x1100, 0x115F) || in_range(cp, 0x2E80, 0x303E) || in_range(cp, 0x3041, 0x33FF) ||
      in_range(cp, 0x3400, 0x4DBF) || in_range(cp, 0x4E00, 0x9FFF) || in_range(cp, 0xA000, 0xA4CF) ||
      in_range(cp, 0xAC00, 0xD7A3) || in_range(cp, 0xF900, 0xFAFF) || in_range(cp, 0xFE30, 0xFE4F) ||
      in_range(cp, 0xFF00, 0xFF60) || in_range(cp, 0xFFE0, 0xFFE6) || in_range(cp, 0x1F300, 0x1F64F) ||
      in_range(cp, 0x1F900, 0x1F9FF) || in_range(cp, 0x20000, 0x3FFFD)) {
    return 2;
  }
  return 1;
}

} // namespace

auto is_format_char(char32_t cp) -> bool {
  return in_range(cp, 0x200B, 0x200F) || in_range(cp, 0x202A, 0x202E) || in_range(cp, 0x2066, 0x2069) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0xFEFF || cp == 0x061C || in_range(cp, 0x2060, 0x2064) || in_range(cp, 0x206A, 0x206F);
}

auto has_hazard(std::string_view text) -> bool {
  for (std::size_t i = 0; i < text.size();) {
    auto const d = decode_at(text, i);
    if (!d.valid || d.value < 0x20 || d.value == 0x7F || is_c1(d.value) || is_format_char(d.value)) {
      return true;
    }
    i += d.length;
  }
  return false;
}

auto quote_text(std::string_view value) -> std::string {
  std::string out = "\"";
  for (std::size_t i = 0; i < value.size();) {
    auto const d = decode_at(value, i);
    if (d.valid && d.value == '\\') {
      out += "\\\\";
    } else if (d.valid && d.value == '"') {
      out += "\\\"";
    } else if (d.valid && d.value == '\n') {
      out += "\\n";
    } else if (d.valid && d.value == '\r') {
      out += "\\r";
    } else if (d.valid && d.value == '\t') {
      out += "\\t";
    } else if (!d.valid) {
      out += std::format("\\x{:02x}", static_cast<unsigned>(d.value));
    } else if (d.valid && (d.value < 0x20 || d.value == 0x7F || is_c1(d.value) || is_format_char(d.value))) {
      out += std::format("\\u{:04x}", static_cast<unsigned>(d.value));
    } else {
      out.append(value.substr(i, d.length));
    }
    i += d.length;
  }
  out += '"';
  return out;
}

auto display_width(std::string_view text) -> std::size_t {
  std::size_t width = 0;
  for (std::size_t i = 0; i < text.size();) {
    auto const d = decode_at(text, i);
    width += d.valid ? cell_width(d.value) : 1;
    i += d.length;
  }
  return width;
}

auto truncate_display(std::string_view text, std::size_t max_width) -> std::string {
  max_width = std::max<std::size_t>(max_width, 1);
  if (display_width(text) <= max_width) {
    return std::string{text};
  }
  auto const  budget = max_width - 1; // the marker takes one column
  std::size_t used   = 0;
  std::size_t end    = 0;
  for (std::size_t i = 0; i < text.size();) {
    auto const d = decode_at(text, i);
    auto const w = d.valid ? cell_width(d.value) : 1;
    if (used + w > budget) {
      break;
    }
    used += w;
    i += d.length;
    end = i;
  }
  std::string out{text.substr(0, end)};
  out += k_truncation_marker;
  return out;
}

auto cap_field(std::string_view text) -> std::string {
  return truncate_display(text, k_field_cap);
}

} // namespace planar::textview

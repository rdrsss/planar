/// @file json_dom.cpp
/// @brief Implementation of `planar.json_dom` (plan 996, task 6190).
///
/// The parser is a straight recursive-descent walk over RFC 8259 with no
/// extensions — `std.json`'s own default posture. The writer is a
/// transcription of `std/json/Stringify.zig`'s `valueStart`/`indent`/
/// `endObject` state machine, verified against captured oracle bytes (see
/// the module interface's header for the capture).

module planar.json_dom;

import std;
import planar.json_text;

namespace planar::json_dom {

using json_text::append_json_string;

namespace {

/// @brief A cursor over the document being parsed.
struct reader {
  std::string_view text;    ///< The whole document.
  std::size_t      pos = 0; ///< The read cursor.

  /// @brief Set when a parse failed because an object repeated a key.
  ///
  /// A side channel rather than an error-set arm, so the twenty existing
  /// `malformed` sites stay untouched. Only `parse_json_reason` reads it;
  /// see this module interface's `json_parse_reason`.
  bool saw_duplicate_field = false;

  /// @brief Whether the cursor is exhausted.
  /// @return `true` at end of input.
  [[nodiscard]] auto eof() const -> bool {
    return pos >= text.size();
  }

  /// @brief Peek the byte under the cursor.
  /// @return The byte, or `\0` at end of input.
  [[nodiscard]] auto peek() const -> char {
    return eof() ? '\0' : text[pos];
  }

  /// @brief Skip RFC 8259 whitespace (space, tab, CR, LF — and nothing else;
  /// a form feed or vertical tab is a syntax error, same as `std.json`).
  auto skip_ws() -> void {
    while (!eof()) {
      char const c = text[pos];
      if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
        break;
      }
      ++pos;
    }
  }
};

auto parse_value(reader& r, std::size_t depth) -> std::expected<json_value, json_parse_error>;

/// @brief Consume the literal `word`.
/// @param r The cursor.
/// @param word The expected literal.
/// @return Whether it matched (and was consumed).
auto take_literal(reader& r, std::string_view word) -> bool {
  if (r.text.substr(r.pos).starts_with(word)) {
    r.pos += word.size();
    return true;
  }
  return false;
}

/// @brief Append the UTF-8 encoding of `cp` to `out`.
/// @param out The buffer.
/// @param cp The code point.
auto append_utf8(std::string& out, char32_t cp) -> void {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

/// @brief Read four hex digits as a UTF-16 code unit.
/// @param r The cursor, positioned at the first digit.
/// @return The code unit, or nullopt when fewer than four hex digits follow.
auto take_hex4(reader& r) -> std::optional<std::uint16_t> {
  if (r.pos + 4 > r.text.size()) {
    return std::nullopt;
  }
  std::uint16_t value = 0;
  for (int i = 0; i < 4; ++i) {
    char const c = r.text[r.pos + static_cast<std::size_t>(i)];
    int        digit{};
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return std::nullopt;
    }
    value = static_cast<std::uint16_t>((value << 4) | static_cast<std::uint16_t>(digit));
  }
  r.pos += 4;
  return value;
}

/// @brief Parse a JSON string body, cursor positioned ON the opening quote.
/// @param r The cursor.
/// @return The decoded bytes.
auto parse_string(reader& r) -> std::expected<std::string, json_parse_error> {
  if (r.peek() != '"') {
    return std::unexpected(json_parse_error::malformed);
  }
  ++r.pos;
  std::string out;
  while (true) {
    if (r.eof()) {
      return std::unexpected(json_parse_error::malformed);
    }
    char const c = r.text[r.pos];
    if (c == '"') {
      ++r.pos;
      return out;
    }
    if (static_cast<unsigned char>(c) < 0x20) {
      // Unescaped control character — a syntax error in RFC 8259.
      return std::unexpected(json_parse_error::malformed);
    }
    if (c != '\\') {
      out += c;
      ++r.pos;
      continue;
    }
    ++r.pos;
    if (r.eof()) {
      return std::unexpected(json_parse_error::malformed);
    }
    char const esc = r.text[r.pos++];
    switch (esc) {
    case '"':
      out += '"';
      break;
    case '\\':
      out += '\\';
      break;
    case '/':
      out += '/';
      break;
    case 'b':
      out += '\b';
      break;
    case 'f':
      out += '\f';
      break;
    case 'n':
      out += '\n';
      break;
    case 'r':
      out += '\r';
      break;
    case 't':
      out += '\t';
      break;
    case 'u': {
      auto const unit = take_hex4(r);
      if (!unit.has_value()) {
        return std::unexpected(json_parse_error::malformed);
      }
      char32_t cp = *unit;
      if (cp >= 0xD800 && cp <= 0xDBFF) {
        // High surrogate — a low surrogate must follow to form a pair.
        if (r.pos + 2 <= r.text.size() && r.text[r.pos] == '\\' && r.text[r.pos + 1] == 'u') {
          auto const save = r.pos;
          r.pos += 2;
          auto const low = take_hex4(r);
          if (low.has_value() && *low >= 0xDC00 && *low <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (*low - 0xDC00);
          } else {
            r.pos = save;
          }
        }
      }
      append_utf8(out, cp);
      break;
    }
    default:
      return std::unexpected(json_parse_error::malformed);
    }
  }
}

/// @brief Parse a JSON number.
///
/// The token is scanned, then classified exactly the way
/// `std.json.Value.parseFromNumberSlice` classifies it
/// (`std/json/dynamic.zig:30-47`) — three arms, not two:
///
///   - integer-formatted (no `.`, `e`, `E`) AND fits `i64` -> `integer`;
///   - otherwise parses to a FINITE double            -> `floating`;
///   - anything else (i64 overflow, or a double that
///     overflows to infinity)                         -> `number_raw`,
///     the token's SOURCE TEXT, re-emitted verbatim.
///
/// The third arm is the one an obvious implementation drops, and dropping
/// it is not benign: `12345678901234567890` came back as
/// `12345678901234567168` from a draft that folded it into `floating`.
/// See json_dom.cppm's header, item 2.
/// @param r The cursor.
/// @return The parsed number.
auto parse_number(reader& r) -> std::expected<json_value, json_parse_error> {
  auto const start = r.pos;
  if (r.peek() == '-') {
    ++r.pos;
  }
  // Integer part: either a lone `0` or [1-9][0-9]*. Leading zeros are a
  // syntax error, per RFC 8259 and `std.json`.
  if (r.peek() == '0') {
    ++r.pos;
  } else if (r.peek() >= '1' && r.peek() <= '9') {
    while (r.peek() >= '0' && r.peek() <= '9') {
      ++r.pos;
    }
  } else {
    return std::unexpected(json_parse_error::malformed);
  }

  bool integral = true;
  if (r.peek() == '.') {
    integral = false;
    ++r.pos;
    if (!(r.peek() >= '0' && r.peek() <= '9')) {
      return std::unexpected(json_parse_error::malformed);
    }
    while (r.peek() >= '0' && r.peek() <= '9') {
      ++r.pos;
    }
  }
  if (r.peek() == 'e' || r.peek() == 'E') {
    integral = false;
    ++r.pos;
    if (r.peek() == '+' || r.peek() == '-') {
      ++r.pos;
    }
    if (!(r.peek() >= '0' && r.peek() <= '9')) {
      return std::unexpected(json_parse_error::malformed);
    }
    while (r.peek() >= '0' && r.peek() <= '9') {
      ++r.pos;
    }
  }

  auto const token   = r.text.substr(start, r.pos - start);
  auto const raw_arm = [&] { return json_value{.kind = json_kind::number_raw, .string = std::string{token}}; };

  if (integral) {
    std::int64_t value{};
    auto const [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec == std::errc{} && ptr == token.data() + token.size()) {
      return json_value{.kind = json_kind::integer, .integer = value};
    }
    // Integer-formatted but wider than i64 — zig's `error.Overflow` arm.
    // The token is preserved EXACTLY, not narrowed to a double.
    return raw_arm();
  }

  double value{};
  auto const [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
  if (ec == std::errc::result_out_of_range) {
    // Overflowed to infinity. zig keeps the source text here too, because
    // `inf` is not emittable JSON.
    return raw_arm();
  }
  if (ec != std::errc{} || ptr != token.data() + token.size()) {
    return std::unexpected(json_parse_error::malformed);
  }
  if (!std::isfinite(value)) {
    return raw_arm();
  }
  return json_value{.kind = json_kind::floating, .floating = value};
}

/// @brief Parse any JSON value at the cursor.
/// @param r The cursor.
/// @param depth How many containers are already open. See `k_max_depth`.
/// @return The value.
auto parse_value(reader& r, std::size_t depth) -> std::expected<json_value, json_parse_error> {
  if (depth > k_max_depth) {
    // A loud refusal rather than a stack overflow. Documented divergence
    // from the oracle, which uses a heap stack — see json_dom.cppm.
    return std::unexpected(json_parse_error::malformed);
  }
  r.skip_ws();
  if (r.eof()) {
    return std::unexpected(json_parse_error::malformed);
  }
  switch (r.peek()) {
  case 'n':
    if (!take_literal(r, "null")) {
      return std::unexpected(json_parse_error::malformed);
    }
    return json_value{.kind = json_kind::null_};
  case 't':
    if (!take_literal(r, "true")) {
      return std::unexpected(json_parse_error::malformed);
    }
    return json_value{.kind = json_kind::boolean, .boolean = true};
  case 'f':
    if (!take_literal(r, "false")) {
      return std::unexpected(json_parse_error::malformed);
    }
    return json_value{.kind = json_kind::boolean, .boolean = false};
  case '"': {
    auto text = parse_string(r);
    if (!text.has_value()) {
      return std::unexpected(text.error());
    }
    return json_value{.kind = json_kind::string, .string = std::move(*text)};
  }
  case '[': {
    ++r.pos;
    json_value out{.kind = json_kind::array};
    r.skip_ws();
    if (r.peek() == ']') {
      ++r.pos;
      return out;
    }
    while (true) {
      auto item = parse_value(r, depth + 1);
      if (!item.has_value()) {
        return std::unexpected(item.error());
      }
      out.array.push_back(std::move(*item));
      r.skip_ws();
      if (r.peek() == ',') {
        ++r.pos;
        continue;
      }
      if (r.peek() == ']') {
        ++r.pos;
        return out;
      }
      return std::unexpected(json_parse_error::malformed);
    }
  }
  case '{': {
    ++r.pos;
    json_value out{.kind = json_kind::object};
    r.skip_ws();
    if (r.peek() == '}') {
      ++r.pos;
      return out;
    }
    while (true) {
      r.skip_ws();
      auto key = parse_string(r);
      if (!key.has_value()) {
        return std::unexpected(key.error());
      }
      r.skip_ws();
      if (r.peek() != ':') {
        return std::unexpected(json_parse_error::malformed);
      }
      ++r.pos;
      auto val = parse_value(r, depth + 1);
      if (!val.has_value()) {
        return std::unexpected(val.error());
      }
      // A DUPLICATE KEY IS AN ERROR. `std.json.ParseOptions`'s
      // `duplicate_field_behavior` defaults to `.@"error"`, and nothing
      // in the oracle overrides it. A draft of this file reasoned from
      // `ObjectMap.put` instead and implemented "last value at the first
      // position"; the differential caught it — the oracle REFUSES
      // `{"dup":"a","dup":"b"}` and the draft rendered `"b"`.
      auto const duplicate = std::ranges::any_of(out.object, [&](auto const& kv) { return kv.first == *key; });
      if (duplicate) {
        r.saw_duplicate_field = true;
        return std::unexpected(json_parse_error::malformed);
      }
      out.object.emplace_back(std::move(*key), std::move(*val));
      r.skip_ws();
      if (r.peek() == ',') {
        ++r.pos;
        continue;
      }
      if (r.peek() == '}') {
        ++r.pos;
        return out;
      }
      return std::unexpected(json_parse_error::malformed);
    }
  }
  default:
    return parse_number(r);
  }
}

/// @brief Write `level` levels of two-space indent, preceded by a newline —
/// zig's `indent()` with `.indent_2`.
/// @param out The buffer.
/// @param level The nesting depth.
auto write_indent(std::string& out, std::size_t level) -> void {
  out += '\n';
  out.append(level * 2, ' ');
}

/// @brief Format a `double` the way zig's `{d}` does: shortest
/// round-trippable decimal.
/// @param value The number.
/// @return The formatted text.
auto format_double(double value) -> std::string {
  std::array<char, 64> buf{};
  auto const [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
  if (ec != std::errc{}) {
    return "0";
  }
  return std::string{buf.data(), ptr};
}

/// @brief Recursive half of `stringify_indent2`.
/// @param out The buffer.
/// @param value The value to write.
/// @param level The current nesting depth.
auto write_value(std::string& out, const json_value& value, std::size_t level) -> void {
  switch (value.kind) {
  case json_kind::null_:
    out += "null";
    return;
  case json_kind::boolean:
    out += value.boolean ? "true" : "false";
    return;
  case json_kind::integer:
    out += std::to_string(value.integer);
    return;
  case json_kind::floating:
    out += format_double(value.floating);
    return;
  // Verbatim source text -- zig prints `.number_string` with `{s}`.
  case json_kind::number_raw:
    out += value.string;
    return;
  case json_kind::string:
    append_json_string(out, value.string);
    return;
  case json_kind::array: {
    // Empty containers stay INLINE — `[]`, no newline, no indent. A
    // non-empty one always breaks. Verified against the oracle's
    // `"assignees": []` beside its multi-line `"labels"`.
    if (value.array.empty()) {
      out += "[]";
      return;
    }
    out += '[';
    for (std::size_t i = 0; i < value.array.size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      write_indent(out, level + 1);
      write_value(out, value.array[i], level + 1);
    }
    write_indent(out, level);
    out += ']';
    return;
  }
  case json_kind::object: {
    if (value.object.empty()) {
      out += "{}";
      return;
    }
    out += '{';
    for (std::size_t i = 0; i < value.object.size(); ++i) {
      if (i > 0) {
        out += ',';
      }
      write_indent(out, level + 1);
      append_json_string(out, value.object[i].first);
      out += ": ";
      write_value(out, value.object[i].second, level + 1);
    }
    write_indent(out, level);
    out += '}';
    return;
  }
  }
}

} // namespace

auto json_value::find(std::string_view key) const -> const json_value* {
  if (kind != json_kind::object) {
    return nullptr;
  }
  auto it = std::ranges::find_if(object, [&](auto const& kv) { return kv.first == key; });
  return it == object.end() ? nullptr : &it->second;
}

auto parse_json_reason(std::string_view text) -> std::expected<json_value, json_parse_reason> {
  reader r{.text = text};
  auto   value = parse_value(r, 0);
  if (value.has_value()) {
    r.skip_ws();
    if (r.eof()) {
      return *std::move(value);
    }
    // Trailing garbage. `std.json.parseFromSlice` rejects this; glaze's
    // `read_json` would not. See this module interface's header. Input
    // REMAINS, so this classifies as `syntax`, matching the oracle's
    // `SyntaxError` on `{} trailing`.
    return std::unexpected(json_parse_reason::syntax);
  }

  if (r.saw_duplicate_field) {
    return std::unexpected(json_parse_reason::duplicate_field);
  }
  // The whole classification, in ONE place rather than at each of the
  // twenty failure sites: a parse that stopped with nothing left to read
  // ran OUT of input; one that stopped with bytes remaining choked on a
  // byte. Every oracle capture fits — see this module interface's
  // `json_parse_reason`.
  r.skip_ws();
  return std::unexpected(r.eof() ? json_parse_reason::end_of_input : json_parse_reason::syntax);
}

auto parse_json(std::string_view text) -> std::expected<json_value, json_parse_error> {
  auto value = parse_json_reason(text);
  if (value.has_value()) {
    return *std::move(value);
  }
  return std::unexpected(json_parse_error::malformed);
}

auto stringify_indent2(const json_value& value) -> std::string {
  std::string out;
  write_value(out, value, 0);
  return out;
}

} // namespace planar::json_dom

/// @file render.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.queue.render`.
/// See render.cppm for the contract.

module planar.cmd.planar_watch.handlers.queue.render;

import std;
import planar.textview;

namespace planar::cmd::watch::handlers::queue_render {

namespace {

/// @brief Whether the bytes at `at` are a C1 control character (U+0080 to
/// U+009F), which UTF-8 spells `C2 80` to `C2 9F`.
auto is_c1_at(std::string_view text, std::size_t at) -> bool {
  return static_cast<unsigned char>(text[at]) == 0xC2 && at + 1 < text.size() &&
         static_cast<unsigned char>(text[at + 1]) >= 0x80 && static_cast<unsigned char>(text[at + 1]) <= 0x9F;
}

/// @brief One argv word as a shell would need it: bare when only safe
/// characters, single-quoted otherwise, and double-quoted with escapes when it
/// holds a control byte, a format character or invalid UTF-8 (no shell
/// quoting can carry one on a single line, so that form is display text, not a
/// pasteable shell word).
auto shell_word(std::string_view word) -> std::string {
  if (textview::has_hazard(word)) {
    return textview::quote_text(word);
  }
  auto const safe = !word.empty() && std::ranges::all_of(word, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || std::string_view{"_@%+=:,./-"}.contains(c);
  });
  if (safe) {
    return std::string{word};
  }
  std::string out = "'";
  for (auto const c : word) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

} // namespace

auto quote(std::string_view value) -> std::string {
  std::string out = "\"";
  for (std::size_t i = 0; i < value.size(); ++i) {
    auto const c = value[i];
    auto const u = static_cast<unsigned char>(c);
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '"') {
      out += "\\\"";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (u < 0x20 || u == 0x7f) {
      out += std::format("\\u{:04x}", static_cast<unsigned>(u));
    } else if (is_c1_at(value, i)) {
      out += std::format("\\u{:04x}", static_cast<unsigned>(static_cast<unsigned char>(value[i + 1])));
      ++i;
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

auto cell(std::string_view value) -> std::string {
  if (value.empty()) {
    return "-";
  }
  auto const risky =
      textview::has_hazard(value) || value == "-" || value.contains(' ') || value.contains('"') || value.contains('\\');
  return risky ? textview::quote_text(value) : std::string{value};
}

auto field_cell(std::string_view value) -> std::string {
  return cell(textview::cap_field(value));
}

auto shell_line(const std::vector<std::string>& argv) -> std::string {
  std::string out;
  for (auto const& word : argv) {
    if (!out.empty()) {
      out += ' ';
    }
    out += shell_word(word);
  }
  return out.empty() ? "-" : out;
}

auto span_ms(std::int64_t from, std::int64_t to) -> std::int64_t {
  return std::max<std::int64_t>(0, to - from);
}

auto duration_text(const std::optional<std::int64_t>& ms) -> std::string {
  if (!ms) {
    return "-";
  }
  auto const seconds = *ms / 1000;
  if (seconds < 60) {
    return std::format("{}s", seconds);
  }
  if (seconds < 3600) {
    return std::format("{}m{:02}s", seconds / 60, seconds % 60);
  }
  return std::format("{}h{:02}m", seconds / 3600, (seconds % 3600) / 60);
}

auto pad_table(const std::vector<std::vector<std::string>>& table) -> std::string {
  if (table.empty()) {
    return {};
  }
  auto const               columns = table.front().size();
  std::vector<std::size_t> width(columns, 0);
  for (auto const& line : table) {
    for (std::size_t i = 0; i + 1 < columns; ++i) {
      width[i] = std::max(width[i], textview::display_width(line[i]));
    }
  }
  std::string out;
  for (auto const& line : table) {
    for (std::size_t i = 0; i + 1 < columns; ++i) {
      out += line[i];
      out.append(width[i] - textview::display_width(line[i]) + 2, ' ');
    }
    out += line.back();
    out += '\n';
  }
  return out;
}

void put_key(std::string& out, std::string_view key) {
  out += out.back() == '{' ? "" : ",";
  out += quote(key);
  out += ':';
}

void put_int(std::string& out, std::string_view key, const std::optional<std::int64_t>& value) {
  put_key(out, key);
  out += value ? std::to_string(*value) : "null";
}

void put_text(std::string& out, std::string_view key, const std::optional<std::string>& value) {
  put_key(out, key);
  out += value ? quote(*value) : "null";
}

void put_argv(std::string& out, const std::vector<std::string>& argv) {
  put_key(out, "argv");
  out += '[';
  for (std::size_t i = 0; i < argv.size(); ++i) {
    out += i == 0 ? "" : ",";
    out += quote(argv[i]);
  }
  out += ']';
}

} // namespace planar::cmd::watch::handlers::queue_render

/// @file docs_manifest.cpp
/// @brief Implementation of `planar.docs_manifest` (see docs_manifest.cppm).

module;

#include <xxhash.h>

module planar.docs_manifest;

import std;
import planar.json_text;

namespace planar::docs_manifest {

namespace {

/// @brief `manifest.zig`'s `trimRight`: strip trailing characters in `values`.
auto trim_right(std::string_view s, std::string_view values) -> std::string_view {
  auto end = s.size();
  while (end > 0 && values.find(s[end - 1]) != std::string_view::npos) {
    --end;
  }
  return s.substr(0, end);
}

/// @brief `manifest.zig`'s `writeFrontMatterNormalized`: if `content` opens
/// with a `---\n` ... `\n---\n` front-matter block, strip any
/// `regenerated_at:` / `source_versions:` line from it (after per-line
/// right-trim) and re-emit the block; otherwise return `content` untouched.
/// @return The body AFTER the front-matter block (or the whole input when
/// there was none), plus whatever was written into `out` for the block itself.
auto write_front_matter_normalized(std::string& out, std::string_view content) -> std::string_view {
  if (!content.starts_with("---\n")) {
    return content;
  }
  const auto rest = content.substr(4);
  const auto idx  = rest.find("\n---\n");
  if (idx == std::string_view::npos) {
    return content;
  }
  const auto fm   = rest.substr(0, idx);
  const auto body = rest.substr(idx + 5);

  out += "---\n";
  std::size_t pos = 0;
  while (pos <= fm.size()) {
    const auto nl      = fm.find('\n', pos);
    const auto line    = nl == std::string_view::npos ? fm.substr(pos) : fm.substr(pos, nl - pos);
    const auto trimmed = trim_right(line, " \t");
    // `key` left-AND-right trims `trimmed` purely to test the two skipped
    // prefixes; the line actually WRITTEN is `trimmed` (right-trim only),
    // matching `manifest.zig`'s `writeFrontMatterNormalized` exactly —
    // leading whitespace on a kept line is preserved.
    const auto lead = trimmed.find_first_not_of(" \t");
    const auto key  = lead == std::string_view::npos ? std::string_view{} : trimmed.substr(lead);
    if (!key.starts_with("regenerated_at:") && !key.starts_with("source_versions:")) {
      out += trimmed;
      out += '\n';
    }
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  out += "---\n";
  return body;
}

} // namespace

auto normalize(std::string_view content) -> std::string {
  std::string folded;
  folded.reserve(content.size());
  for (std::size_t i = 0; i < content.size(); ++i) {
    if (content[i] == '\r') {
      if (i + 1 < content.size() && content[i + 1] == '\n') {
        ++i;
      }
      folded += '\n';
    } else {
      folded += content[i];
    }
  }

  std::string front_matter_out;
  const auto  body = write_front_matter_normalized(front_matter_out, folded);

  std::string out = std::move(front_matter_out);
  std::size_t pos = 0;
  while (pos <= body.size()) {
    const auto nl   = body.find('\n', pos);
    const auto line = nl == std::string_view::npos ? body.substr(pos) : body.substr(pos, nl - pos);
    out += trim_right(line, " \t");
    out += '\n';
    if (nl == std::string_view::npos) {
      break;
    }
    pos = nl + 1;
  }
  while (out.size() > 1 && out[out.size() - 1] == '\n' && out[out.size() - 2] == '\n') {
    out.pop_back();
  }
  if (out.empty() || out.back() != '\n') {
    out += '\n';
  }
  return out;
}

auto hash_hex(std::string_view content) -> std::string {
  const auto sum = XXH64(content.data(), content.size(), 0);
  return std::format("{:016x}", sum);
}

namespace {

auto canonical_sources_json(std::span<const source_row> sources) -> std::string {
  std::string out = "{";
  for (std::size_t i = 0; i < sources.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    json_text::append_json_string(out, sources[i].ref);
    out += ":";
    json_text::append_json_string(out, sources[i].hash);
  }
  out += "}";
  return out;
}

auto build_entry(std::string_view raw) -> entry {
  const auto doc_hash     = hash_hex(normalize(raw));
  const auto sources_json = canonical_sources_json(std::span<const source_row>{});
  const auto sources_hash = hash_hex(sources_json);
  return entry{
      .doc_hash     = doc_hash,
      .sources      = {},
      .sources_hash = sources_hash,
      .entry_hash   = hash_hex(std::format("{}|{}", doc_hash, sources_hash)),
  };
}

auto ends_with_ignore_case(std::string_view name, std::string_view suffix) -> bool {
  if (name.size() < suffix.size()) {
    return false;
  }
  const auto tail = name.substr(name.size() - suffix.size());
  return std::ranges::equal(tail, suffix, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
}

/// @brief Port of `manifest.zig`'s `walkMarkdown`: recurse into every
/// subdirectory, hash every `*.md` file (case-insensitive), and append one
/// `entry_row` per file. Returns false on any filesystem or read failure.
auto walk_markdown(const std::filesystem::path& dir_path, std::vector<entry_row>& rows) -> bool {
  std::error_code                     ec;
  std::filesystem::directory_iterator it(dir_path, ec);
  if (ec) {
    return false;
  }
  for (const auto& item : it) {
    std::error_code kind_ec;
    if (item.is_directory(kind_ec)) {
      if (!walk_markdown(item.path(), rows)) {
        return false;
      }
      continue;
    }
    if (kind_ec) {
      return false;
    }
    std::error_code file_ec;
    if (!item.is_regular_file(file_ec) || file_ec) {
      continue;
    }
    const auto name = item.path().filename().string();
    if (!ends_with_ignore_case(name, ".md")) {
      continue;
    }
    std::ifstream input(item.path(), std::ios::binary);
    if (!input) {
      return false;
    }
    const std::string raw{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    rows.push_back(entry_row{.path = item.path().string(), .value = build_entry(raw)});
  }
  return true;
}

auto compute_root(std::span<const entry_row> rows) -> std::string {
  std::string buf;
  for (const auto& row : rows) {
    buf += row.path;
    buf += '\0';
    buf += row.value.entry_hash;
    buf += '\n';
  }
  return hash_hex(buf);
}

} // namespace

auto build(const std::filesystem::path& root) -> std::optional<manifest> {
  std::vector<entry_row> rows;
  if (!walk_markdown(root, rows)) {
    return std::nullopt;
  }
  std::ranges::sort(rows, {}, &entry_row::path);
  return manifest{
      .version      = docs_manifest::version,
      .algo         = std::string{docs_manifest::algo},
      .root         = compute_root(rows),
      .generated_at = "now", // Literal string, matching the oracle. See docs_manifest.cppm's header.
      .entries      = std::move(rows),
  };
}

auto write(const std::filesystem::path& path, const manifest& value) -> bool {
  std::string body;
  body += std::format("{{\"version\":{},\"algo\":", value.version);
  json_text::append_json_string(body, value.algo);
  body += ",\"root\":";
  json_text::append_json_string(body, value.root);
  body += ",\"generated_at\":";
  json_text::append_json_string(body, value.generated_at);
  body += ",\"entries\":{";
  for (std::size_t i = 0; i < value.entries.size(); ++i) {
    if (i > 0) {
      body += ",";
    }
    const auto& row = value.entries[i];
    json_text::append_json_string(body, row.path);
    body += ":{\"doc_hash\":";
    json_text::append_json_string(body, row.value.doc_hash);
    body += ",\"sources\":{";
    for (std::size_t j = 0; j < row.value.sources.size(); ++j) {
      if (j > 0) {
        body += ",";
      }
      json_text::append_json_string(body, row.value.sources[j].ref);
      body += ":";
      json_text::append_json_string(body, row.value.sources[j].hash);
    }
    body += "},\"sources_hash\":";
    json_text::append_json_string(body, row.value.sources_hash);
    body += ",\"entry_hash\":";
    json_text::append_json_string(body, row.value.entry_hash);
    body += "}";
  }
  body += "}\n}\n";

  std::error_code ec;
  if (auto parent = path.parent_path(); !parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return false;
    }
  }
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream output(tmp, std::ios::binary | std::ios::trunc);
    if (!output) {
      return false;
    }
    output.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!output) {
      return false;
    }
  }
  std::filesystem::rename(tmp, path, ec);
  return !ec;
}

} // namespace planar::docs_manifest

/// @file src/cmd/planar/handlers/update/state.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.update.state`.

module;

// Glaze is not a module; it comes in through the global module fragment.
#include <glaze/glaze.hpp>

#include <sys/stat.h>
#include <unistd.h>

module planar.cmd.planar.handlers.update.state;

import std;

namespace planar::cmd::update {

namespace {

/// @brief Split `text` into lines the way a `while IFS= read -r line || [ -n
/// "$line" ]` loop does: a final line without a newline counts when it is not
/// empty, and a trailing newline adds no empty line.
/// @param text The file body.
/// @return The lines, without their newlines.
auto read_lines(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> lines;
  while (!text.empty()) {
    auto const nl = text.find('\n');
    if (nl == std::string_view::npos) {
      lines.push_back(text);
      break;
    }
    lines.push_back(text.substr(0, nl));
    text.remove_prefix(nl + 1);
  }
  return lines;
}

auto is_digits(std::string_view s) -> bool {
  return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
}

auto is_lower_alpha(char c) -> bool {
  return c >= 'a' && c <= 'z';
}

auto is_alnum(char c) -> bool {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

/// @brief Read a whole file; unset when it cannot be opened.
auto slurp(const std::filesystem::path& path) -> std::optional<std::string> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream body;
  body << in.rdbuf();
  return std::move(body).str();
}

/// @brief `MAJOR.MINOR[.PATCH]`, each 1..6 digits, as (major, minor).
auto version_parts(std::string_view v) -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
  std::vector<std::string_view> parts;
  for (auto const piece : std::views::split(v, '.')) {
    parts.emplace_back(piece.begin(), piece.end());
  }
  if (parts.size() != 2 && parts.size() != 3) {
    return std::nullopt;
  }
  for (auto const& p : parts) {
    if (!is_digits(p) || p.size() > 6) {
      return std::nullopt;
    }
  }
  std::uint64_t major = 0;
  std::uint64_t minor = 0;
  std::from_chars(parts[0].data(), parts[0].data() + parts[0].size(), major);
  std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), minor);
  return std::pair{major, minor};
}

} // namespace

auto canonical_path(std::string_view input, std::string_view cwd) -> std::optional<std::string> {
  if (input.empty()) {
    return std::nullopt;
  }
  std::string remaining = input.starts_with('/') ? std::string{input} : std::format("{}/{}", cwd, input);
  std::string resolved  = "/";
  int         links     = 0;
  while (!remaining.empty()) {
    std::string comp;
    if (auto const slash = remaining.find('/'); slash != std::string::npos) {
      comp      = remaining.substr(0, slash);
      remaining = remaining.substr(slash + 1);
    } else {
      comp = std::exchange(remaining, std::string{});
    }
    if (comp.empty() || comp == ".") {
      continue;
    }
    if (comp == "..") {
      resolved = resolved.substr(0, resolved.rfind('/'));
      if (resolved.empty()) {
        resolved = "/";
      }
      continue;
    }
    std::string const target = resolved == "/" ? "/" + comp : resolved + "/" + comp;
    struct stat       st{};
    if (::lstat(target.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
      if (++links > 40) {
        return std::nullopt;
      }
      std::array<char, 4096> buf{};
      auto const             n = ::readlink(target.c_str(), buf.data(), buf.size());
      if (n < 0 || static_cast<std::size_t>(n) >= buf.size()) {
        return std::nullopt;
      }
      std::string const link(buf.data(), static_cast<std::size_t>(n));
      if (link.starts_with('/')) {
        resolved = "/";
      }
      remaining = link + "/" + remaining;
      continue;
    }
    resolved = target;
  }
  return resolved;
}

auto journal::value(std::string_view key) const -> std::string {
  auto const found = keys.find(key);
  return found == keys.end() ? std::string{} : found->second;
}

auto read_journal(std::string_view canonical_root) -> journal_reading {
  std::string root{canonical_root};
  while (root.size() > 1 && root.ends_with('/')) {
    root.pop_back();
  }
  auto const  path = std::format("{}/{}", root, k_journal_name);
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) {
    return {.status = journal_status::absent};
  }
  journal_reading const invalid{.status = journal_status::invalid};
  if (!S_ISREG(st.st_mode) || st.st_uid != ::geteuid() || st.st_size > 65536) {
    return invalid;
  }
  auto const body = slurp(path);
  if (!body.has_value() || body->size() > 65536) {
    return invalid;
  }
  for (char const c : *body) {
    auto const u = static_cast<unsigned char>(c);
    if (!(u == '\t' || u == '\n' || (u >= 0x20 && u <= 0x7E) || u >= 0x80)) {
      return invalid;
    }
  }
  auto const lines = read_lines(*body);
  if (lines.empty() || lines.front() != "planar-journal 1") {
    return invalid;
  }
  journal record;
  for (auto const line : lines | std::views::drop(1)) {
    auto const eq = line.find('=');
    if (line.empty() || !is_lower_alpha(line.front()) || eq == std::string_view::npos) {
      return invalid;
    }
    auto const key = line.substr(0, eq);
    if (!std::ranges::all_of(key, [](char c) { return is_lower_alpha(c) || (c >= '0' && c <= '9') || c == '_'; })) {
      return invalid;
    }
    if (!record.keys.emplace(std::string{key}, std::string{line.substr(eq + 1)}).second) {
      return invalid;
    }
  }
  auto const r = record.keys.find("root");
  auto const p = record.keys.find("phase");
  if (r == record.keys.end() || p == record.keys.end() || r->second != root) {
    return invalid;
  }
  static constexpr std::array<std::string_view, 5> k_phases{"prepared", "mutating", "complete", "aborted-before-mutation",
                                                            "uninstalling"};
  if (std::ranges::find(k_phases, p->second) == k_phases.end()) {
    return invalid;
  }
  record.phase = p->second;
  return {.status = journal_status::valid, .record = std::move(record)};
}

auto read_release(const std::filesystem::path& path) -> std::expected<std::optional<release_info>, std::string> {
  std::error_code ec;
  if (!std::filesystem::exists(std::filesystem::symlink_status(path, ec))) {
    return std::optional<release_info>{};
  }
  auto const body = slurp(path);
  if (!body.has_value()) {
    return std::unexpected(std::format("cannot read {}", path.string()));
  }
  auto parsed = glz::read_json<glz::generic>(*body);
  if (!parsed || !parsed->is_object()) {
    return std::unexpected(std::format("{} is not a JSON object", path.string()));
  }
  glz::generic const& root = *parsed;
  auto const          text = [&](std::string_view key) -> std::optional<std::string> {
    std::string const k{key};
    if (root.contains(k) && root.at(k).is_string()) {
      return root.at(k).get<std::string>();
    }
    return std::nullopt;
  };
  auto version = text("version");
  if (!version.has_value()) {
    return std::unexpected(std::format("{} records no release version", path.string()));
  }
  release_info info{.version = std::move(*version), .os_floor = text("os_floor").value_or(""), .sha = text("sha").value_or("")};
  if (root.contains("schema_version")) {
    auto const& sv = root.at("schema_version");
    if (sv.is_number()) {
      auto const d = sv.get<double>();
      if (d >= 0 && d == std::floor(d) && d < 1e15) {
        info.schema_version = static_cast<std::uint64_t>(d);
      }
    } else if (sv.is_string()) {
      auto const&   s = sv.get<std::string>();
      std::uint64_t n = 0;
      if (is_digits(s) && s.size() <= 15 && std::from_chars(s.data(), s.data() + s.size(), n).ec == std::errc{}) {
        info.schema_version = n;
      }
    }
  }
  return std::optional<release_info>{std::move(info)};
}

auto normalize_base(std::string_view base) -> std::string {
  std::string out{base};
  while (out.ends_with('/')) {
    out.pop_back();
  }
  return out;
}

auto release_base_valid(std::string_view url) -> std::optional<base_parts> {
  auto const allowed = [](char c) {
    return is_alnum(c) || c == '.' || c == '_' || c == '~' || c == ':' || c == '/' || c == '%' || c == '+' || c == '-';
  };
  if (url.empty() || !std::ranges::all_of(url, allowed)) {
    return std::nullopt;
  }
  std::string      scheme;
  std::string_view rest;
  for (std::string_view const s : {"https", "http", "file"}) {
    auto const prefix = std::format("{}://", s);
    if (url.starts_with(prefix)) {
      scheme = s;
      rest   = url.substr(prefix.size());
      break;
    }
  }
  if (scheme.empty()) {
    return std::nullopt;
  }
  std::string_view auth = rest;
  std::string      path;
  if (auto const slash = rest.find('/'); slash != std::string_view::npos) {
    auth = rest.substr(0, slash);
    path = std::format("/{}", rest.substr(slash + 1));
  }
  if (scheme == "file") {
    if (!auth.empty() || path.empty()) {
      return std::nullopt;
    }
    if (path.contains('%') || path.contains("/../") || path.ends_with("/..") || path.contains("/./") || path.ends_with("/.")) {
      return std::nullopt;
    }
    return base_parts{.scheme = scheme, .path = path};
  }
  std::string_view host = auth;
  if (auto const colon = auth.find(':'); colon != std::string_view::npos) {
    host            = auth.substr(0, colon);
    auto const port = auth.substr(colon + 1);
    if (!is_digits(port) || port.size() > 5) {
      return std::nullopt;
    }
    std::uint32_t n = 0;
    std::from_chars(port.data(), port.data() + port.size(), n);
    if (n > 65535) {
      return std::nullopt;
    }
  }
  if (host.empty() || !std::ranges::all_of(host, [](char c) { return is_alnum(c) || c == '.' || c == '-'; }) ||
      host.front() == '.' || host.back() == '.' || host.front() == '-' || host.back() == '-' || host.contains("..")) {
    return std::nullopt;
  }
  if (scheme == "http" && host != "127.0.0.1" && host != "localhost") {
    return std::nullopt;
  }
  return base_parts{.scheme = scheme, .path = path};
}

auto release_base_refusal(std::string_view given) -> std::string {
  return std::format("PLANAR_RELEASE_URL='{}' is not an accepted release base; it must be https://HOST[:PORT]/..., "
                     "file:///PATH, or http://127.0.0.1[:PORT]/... or http://localhost[:PORT]/... with no userinfo",
                     printable(given));
}

auto version_valid(std::string_view tag) -> bool {
  if (!tag.starts_with('v')) {
    return false;
  }
  std::vector<std::string_view> parts;
  for (auto const piece : std::views::split(tag.substr(1), '.')) {
    parts.emplace_back(piece.begin(), piece.end());
  }
  return parts.size() == 3 && std::ranges::all_of(parts, is_digits);
}

auto printable(std::string_view text) -> std::string {
  std::string out;
  for (char const c : text) {
    if (c >= 0x20 && c <= 0x7E) {
      out += c;
    }
  }
  if (out.size() > 200) {
    out.resize(200);
  }
  return out;
}

auto select_checksum_record(std::string_view sums, std::string_view asset) -> std::expected<std::string, std::string> {
  std::size_t count = 0;
  std::string record;
  for (auto line : read_lines(sums)) {
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    if (line.ends_with(asset)) {
      ++count;
      record = line;
    }
  }
  if (count == 0) {
    return std::unexpected(std::format("SHA256SUMS has no checksum record for {}; nothing was extracted or installed", asset));
  }
  if (count > 1) {
    return std::unexpected(
        std::format("SHA256SUMS has {} checksum records for {}; nothing was extracted or installed", count, asset));
  }
  auto const hash = record.substr(0, record.find("  "));
  if (record != std::format("{}  {}", hash, asset) || hash.size() != 64) {
    return std::unexpected(
        std::format("the SHA256SUMS record for {} is malformed or names a path; nothing was extracted or installed", asset));
  }
  if (!std::ranges::all_of(hash, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
    return std::unexpected(
        std::format("the SHA256SUMS record for {} holds a malformed hash; nothing was extracted or installed", asset));
  }
  return hash;
}

auto glibc_refusal(std::string_view floor, const std::optional<std::string>& ldd_first_line) -> std::optional<std::string> {
  auto const want = version_parts(floor);
  if (!want.has_value()) {
    return std::format("release.json holds os_floor '{}', which is not a glibc version (MAJOR.MINOR); nothing was installed",
                       printable(floor));
  }
  if (!ldd_first_line.has_value()) {
    return std::format(
        "ldd is not installed, so the glibc version cannot be read; this release needs glibc {} or later; nothing was installed",
        floor);
  }
  std::string_view const line = *ldd_first_line;
  auto const             host = line.substr(line.rfind(' ') == std::string_view::npos ? 0 : line.rfind(' ') + 1);
  auto const             have = version_parts(host);
  if (!have.has_value()) {
    return std::format("cannot read the glibc version from 'ldd --version' (first line: '{}'); this release needs glibc {} or "
                       "later (musl is not supported); nothing was installed",
                       printable(line), floor);
  }
  if (*have < *want) {
    return std::format("this host has glibc {} but this release needs glibc {} or later; nothing was installed", host, floor);
  }
  return std::nullopt;
}

auto platform_for(std::string_view sysname, std::string_view machine) -> std::expected<std::string, std::string> {
  if (sysname == "Darwin" && machine == "arm64") {
    return std::string{"macos-arm64"};
  }
  if (sysname == "Linux" && machine == "x86_64") {
    return std::string{"linux-x86_64"};
  }
  return std::unexpected(std::format("unsupported platform {} {}; Planar release bundles exist for macos-arm64 (macOS 26.0 or "
                                     "later) and linux-x86_64 only. {}",
                                     printable(sysname), printable(machine), k_source_pointer));
}

} // namespace planar::cmd::update

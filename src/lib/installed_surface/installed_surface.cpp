/// @file installed_surface.cpp
/// @brief Implementation of `planar.installed_surface`. See
/// installed_surface.cppm for the classification rules.
///
/// ## Accepted divergence: I/O errors OTHER than "not found" degrade rather
/// than refuse
///
/// The oracle's `readFileAlloc`/`readLink`/directory-`iterate` calls
/// propagate any error besides `FileNotFound` (and, for the manifest read,
/// besides the ones explicitly matched) straight out of `status()` as a hard
/// failure the CLI handler then dies on. This port's `try_read_file` /
/// `path_exists` / the `directory_iterator` construction collapse EVERY
/// failure — permission-denied, `ENOTDIR`, a mid-read disk error — into the
/// same "absent/unavailable" outcome the missing-file arm already produces.
/// The result still classifies (stale/missing/skipped rather than a process
/// abort), which is only observable on a real permission failure that no
/// break probe or scratch-arena fixture in this port's test set can trigger
/// portably. Recorded here rather than fixed silently, per D2: a faithful
/// reproduction of "die on EACCES mid-walk" would mean threading
/// `std::expected` through every filesystem call in this file for a
/// practically-unreachable arm.

module planar.installed_surface;

import std;
import planar.json_dom;

namespace planar::installed_surface {

namespace {

/// @brief Cap on the bytes read for the manifest itself and for any staged
/// / installed projection file, mirroring the oracle's `Limit.limited(16 *
/// 1024 * 1024)`.
constexpr std::uintmax_t k_max_read_bytes = 16ULL * 1024 * 1024;

auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return true;
  }
  const auto st = std::filesystem::symlink_status(path, ec);
  return !ec && st.type() != std::filesystem::file_type::not_found;
}

/// @brief Read a whole file's bytes. `std::nullopt` on any failure — missing,
/// oversized, unreadable, or a directory. Mirrors the oracle's
/// `readFileAlloc` used by both the manifest read and `readProjection`.
auto try_read_file(const std::filesystem::path& path) -> std::optional<std::string> {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::nullopt;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > k_max_read_bytes) {
    return std::nullopt;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string content(static_cast<std::size_t>(size), '\0');
  if (size > 0) {
    in.read(content.data(), static_cast<std::streamsize>(size));
    if (!in) {
      return std::nullopt;
    }
  }
  return content;
}

auto is_vendor(std::string_view value) -> bool {
  return std::ranges::any_of(supported_vendors, [&](auto v) { return v == value; });
}

/// @brief Shell-single-quote `raw` the way the oracle's `bootstrapCommand`
/// does: wrap in single quotes and escape an embedded `'` as `'\''`.
auto shell_quote(std::string_view raw) -> std::string {
  std::string out = "'";
  for (char ch : raw) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out += ch;
    }
  }
  out += "'";
  return out;
}

auto bootstrap_command(std::string_view planar_home) -> std::string {
  return std::format("./install.sh --prefix {}", shell_quote(planar_home));
}

/// @brief The strictly-typed manifest schema, decoded from the DOM so
/// duplicate keys and trailing content are rejected the same way the
/// oracle's `std.json.parseFromSliceLeaky` rejects them (see json_dom's
/// header). `std::nullopt` on any structural mismatch (wrong kind, a
/// missing required field, or — matching `ignore_unknown_fields = false` —
/// an unrecognised key).
struct manifest_row {
  std::string vendor;
  std::string kind;
  std::string name;
  std::string staged_path;
  std::string installed_path;
  std::string install_kind;
  std::string source_digest;
  std::string projection_digest;
};

struct manifest_doc {
  std::uint32_t             version = 0;
  std::string               build_id;
  std::string               install_mode;
  std::vector<std::string>  vendors;
  std::vector<manifest_row> projections;
};

auto as_string(const json_dom::json_value* value) -> std::optional<std::string> {
  if (value == nullptr || value->kind != json_dom::json_kind::string) {
    return std::nullopt;
  }
  return value->string;
}

auto as_uint32(const json_dom::json_value* value) -> std::optional<std::uint32_t> {
  if (value == nullptr || value->kind != json_dom::json_kind::integer) {
    return std::nullopt;
  }
  if (value->integer < 0 || value->integer > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(value->integer);
}

/// @brief Whether `obj`'s member keys are a subset of `allowed` — the
/// `ignore_unknown_fields = false` half of the oracle's contract.
auto only_keys(const json_dom::json_value& obj, std::span<const std::string_view> allowed) -> bool {
  for (auto const& [key, _] : obj.object) {
    if (std::ranges::find(allowed, key) == allowed.end()) {
      return false;
    }
  }
  return true;
}

auto parse_row(const json_dom::json_value& value) -> std::optional<manifest_row> {
  if (value.kind != json_dom::json_kind::object) {
    return std::nullopt;
  }
  static constexpr std::string_view k_keys[] = {"vendor",         "kind",         "name",          "staged_path",
                                                "installed_path", "install_kind", "source_digest", "projection_digest"};
  if (!only_keys(value, k_keys)) {
    return std::nullopt;
  }
  manifest_row row;
  auto         vendor            = as_string(value.find("vendor"));
  auto         kind              = as_string(value.find("kind"));
  auto         name              = as_string(value.find("name"));
  auto         staged            = as_string(value.find("staged_path"));
  auto         installed         = as_string(value.find("installed_path"));
  auto         install_kind      = as_string(value.find("install_kind"));
  auto         source_digest     = as_string(value.find("source_digest"));
  auto         projection_digest = as_string(value.find("projection_digest"));
  if (!vendor || !kind || !name || !staged || !installed || !install_kind || !source_digest || !projection_digest) {
    return std::nullopt;
  }
  row.vendor            = *vendor;
  row.kind              = *kind;
  row.name              = *name;
  row.staged_path       = *staged;
  row.installed_path    = *installed;
  row.install_kind      = *install_kind;
  row.source_digest     = *source_digest;
  row.projection_digest = *projection_digest;
  return row;
}

/// @brief Probe just `{"version": N}` — mirrors the oracle's two-pass parse
/// (probe the version before committing to the full schema).
auto probe_version(const json_dom::json_value& doc) -> std::optional<std::uint32_t> {
  if (doc.kind != json_dom::json_kind::object) {
    return std::nullopt;
  }
  return as_uint32(doc.find("version"));
}

auto parse_manifest_doc(const json_dom::json_value& doc) -> std::optional<manifest_doc> {
  if (doc.kind != json_dom::json_kind::object) {
    return std::nullopt;
  }
  static constexpr std::string_view k_keys[] = {"version", "build_id", "install_mode", "vendors", "extras", "projections"};
  if (!only_keys(doc, k_keys)) {
    return std::nullopt;
  }
  manifest_doc out;
  auto         version      = as_uint32(doc.find("version"));
  auto         build_id     = as_string(doc.find("build_id"));
  auto         install_mode = as_string(doc.find("install_mode"));
  auto const*  vendors      = doc.find("vendors");
  auto const*  projections  = doc.find("projections");
  if (!version || !build_id || !install_mode || vendors == nullptr || vendors->kind != json_dom::json_kind::array ||
      projections == nullptr || projections->kind != json_dom::json_kind::array) {
    return std::nullopt;
  }
  // `extras` is optional; when present it must be a string array, though
  // nothing downstream reads it (matches the oracle's default `&.{}`).
  if (auto const* extras = doc.find("extras"); extras != nullptr) {
    if (extras->kind != json_dom::json_kind::array) {
      return std::nullopt;
    }
    for (auto const& item : extras->array) {
      if (!as_string(&item)) {
        return std::nullopt;
      }
    }
  }
  out.version      = *version;
  out.build_id     = *build_id;
  out.install_mode = *install_mode;
  for (auto const& item : vendors->array) {
    auto v = as_string(&item);
    if (!v) {
      return std::nullopt;
    }
    out.vendors.push_back(*v);
  }
  for (auto const& item : projections->array) {
    auto row = parse_row(item);
    if (!row) {
      return std::nullopt;
    }
    out.projections.push_back(*row);
  }
  return out;
}

auto is_digest(std::string_view value) -> bool {
  // Empty is a valid shape, not just 64-hex: scriptorium-rendered
  // projections carry no in-band digest headers, so the M1 install-manifest
  // writer already emits an absence-tolerant empty string for these fields
  // (plan 918 tech-spec § Architecture "installedsurface.zig"). Manifest
  // structural validation must accept that shape rather than reject it.
  if (value.empty()) {
    return true;
  }
  if (value.size() != 64) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char ch) { return std::isdigit(ch) || (ch >= 'a' && ch <= 'f'); });
}

auto valid_manifest(const manifest_doc& manifest) -> bool {
  if (manifest.build_id.empty()) {
    return false;
  }
  if (manifest.install_mode != "copy" && manifest.install_mode != "link") {
    return false;
  }
  for (std::size_t i = 0; i < manifest.vendors.size(); ++i) {
    if (!is_vendor(manifest.vendors[i])) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (manifest.vendors[j] == manifest.vendors[i]) {
        return false;
      }
    }
  }
  for (std::size_t i = 0; i < manifest.projections.size(); ++i) {
    auto const& row = manifest.projections[i];
    if (!is_vendor(row.vendor) || std::ranges::find(manifest.vendors, row.vendor) == manifest.vendors.end()) {
      return false;
    }
    if ((row.kind != "skill" && row.kind != "agent") || row.name.empty()) {
      return false;
    }
    if (row.staged_path.empty() || row.installed_path.empty()) {
      return false;
    }
    if (row.install_kind != "copy" && row.install_kind != "link") {
      return false;
    }
    if (!is_digest(row.source_digest) || !is_digest(row.projection_digest)) {
      return false;
    }
    for (std::size_t j = 0; j < i; ++j) {
      auto const& prior = manifest.projections[j];
      if (prior.installed_path == row.installed_path) {
        return false;
      }
      if (prior.vendor == row.vendor && prior.kind == row.kind && prior.name == row.name) {
        return false;
      }
    }
  }
  return true;
}

auto classify_row(const manifest_row& row, std::string_view bootstrap) -> projection_status {
  projection_status base{
      .vendor         = row.vendor,
      .kind           = row.kind,
      .name           = row.name,
      .staged_path    = row.staged_path,
      .installed_path = row.installed_path,
      .install_kind   = row.install_kind,
      .status         = state::stale,
      .reason         = "staged projection is unavailable",
      .repair_command = std::string(bootstrap),
  };

  auto staged = try_read_file(row.staged_path);
  if (!staged) {
    return base;
  }

  std::error_code ec;
  const auto      link_target = std::filesystem::is_symlink(row.installed_path, ec) && !ec
                                    ? std::filesystem::read_symlink(row.installed_path, ec)
                                    : std::filesystem::path{};
  const bool      is_link     = !ec && !link_target.empty();

  const auto installed_type = std::filesystem::symlink_status(row.installed_path, ec).type();
  if (ec || installed_type == std::filesystem::file_type::not_found) {
    base.status = state::missing;
    base.reason = "managed installed projection is missing";
    return base;
  }
  if (installed_type == std::filesystem::file_type::directory) {
    base.reason = "managed installed destination is a directory and cannot be replaced safely";
    return base;
  }

  auto installed = try_read_file(row.installed_path);
  if (!installed) {
    base.status = state::missing;
    base.reason = "managed installed projection is missing";
    return base;
  }

  if (row.install_kind == "link") {
    if (!is_link || link_target.string() != row.staged_path) {
      base.reason = "managed link target differs from the install manifest";
      return base;
    }
  } else if (is_link || *staged != *installed) {
    base.reason = "managed installed bytes differ from the staged projection";
    return base;
  }

  base.status         = state::fresh;
  base.reason         = "staged projection and installed projection agree";
  base.repair_command = std::nullopt;
  return base;
}

struct vendor_root {
  std::string path;
  std::string kind;
  bool        directory_shape = false;
};

auto vendor_roots(const options& opts, std::string_view vendor) -> std::array<vendor_root, 2> {
  const std::filesystem::path home(opts.home);
  const std::filesystem::path codex_home(opts.codex_home);
  if (vendor == "claude") {
    return {vendor_root{.path = (home / ".claude" / "commands").string(), .kind = "skill", .directory_shape = false},
            vendor_root{.path = (home / ".claude" / "agents").string(), .kind = "agent", .directory_shape = false}};
  }
  if (vendor == "codex") {
    return {vendor_root{.path = (codex_home / "skills").string(), .kind = "skill", .directory_shape = true},
            vendor_root{.path = (codex_home / "agents").string(), .kind = "agent", .directory_shape = false}};
  }
  if (vendor == "copilot") {
    return {vendor_root{.path = (home / ".copilot" / "skills").string(), .kind = "skill", .directory_shape = true},
            vendor_root{.path = (home / ".copilot" / "agents").string(), .kind = "agent", .directory_shape = false}};
  }
  // gemini
  return {
      vendor_root{.path = (home / ".gemini" / "antigravity-cli" / "skills").string(), .kind = "skill", .directory_shape = true},
      vendor_root{.path = (home / ".gemini" / "antigravity-cli" / "agents").string(), .kind = "agent", .directory_shape = false}};
}

auto manifest_owns(const manifest_doc& manifest, std::string_view installed_path) -> bool {
  return std::ranges::any_of(manifest.projections, [&](auto const& row) { return row.installed_path == installed_path; });
}

auto projection_name(std::string_view entry_name) -> std::string {
  static constexpr std::string_view k_suffixes[] = {".agent.md", ".toml", ".md"};
  for (auto suffix : k_suffixes) {
    if (entry_name.size() >= suffix.size() && entry_name.substr(entry_name.size() - suffix.size()) == suffix) {
      return std::string(entry_name.substr(0, entry_name.size() - suffix.size()));
    }
  }
  return std::string(entry_name);
}

void discover_unmanaged(const options& opts, const manifest_doc& manifest, std::string_view vendor,
                        std::vector<projection_status>& projections, installed_surface::summary& summary) {
  for (auto const& root : vendor_roots(opts, vendor)) {
    std::error_code                     ec;
    std::filesystem::directory_iterator it(root.path, ec);
    if (ec) {
      continue;
    }
    for (auto const& entry : it) {
      auto const name = entry.path().filename().string();
      if (name == "." || name == "..") {
        continue;
      }
      const std::string installed = root.directory_shape ? (entry.path() / "SKILL.md").string() : entry.path().string();
      if (manifest_owns(manifest, installed)) {
        continue;
      }
      if (root.directory_shape && !path_exists(installed)) {
        continue;
      }
      projections.push_back(projection_status{
          .vendor         = std::string(vendor),
          .kind           = root.kind,
          .name           = projection_name(name),
          .staged_path    = "",
          .installed_path = installed,
          .install_kind   = "unmanaged",
          .status         = state::unmanaged,
          .reason         = "destination entry has no install-manifest row",
          .repair_command = std::nullopt,
      });
      ++summary.unmanaged;
    }
  }
}

void increment(installed_surface::summary& summary, state value) {
  switch (value) {
  case state::fresh:
    ++summary.fresh;
    break;
  case state::stale:
    ++summary.stale;
    break;
  case state::missing:
    ++summary.missing;
    break;
  case state::unmanaged:
    ++summary.unmanaged;
    break;
  }
}

auto less_projection(const projection_status& lhs, const projection_status& rhs) -> bool {
  if (lhs.vendor != rhs.vendor) {
    return lhs.vendor < rhs.vendor;
  }
  if (lhs.kind != rhs.kind) {
    return lhs.kind < rhs.kind;
  }
  if (lhs.name != rhs.name) {
    return lhs.name < rhs.name;
  }
  return lhs.installed_path < rhs.installed_path;
}

/// @brief Build the aggregate "manifest absent/legacy/invalid/unsupported"
/// result — every vendor `unselected`, no projections.
auto bootstrap_result(std::string manifest_path, std::string bootstrap, manifest_state manifest_status, std::string_view reason,
                      const std::optional<std::string>& vendor_filter) -> status_result {
  status_result out;
  out.manifest_status = manifest_status;
  out.manifest_path   = std::move(manifest_path);
  out.reason          = std::string(reason);
  out.repair_command  = bootstrap;
  for (auto vendor : supported_vendors) {
    if (vendor_filter && *vendor_filter != vendor) {
      continue;
    }
    out.vendors.push_back(vendor_status{.vendor = std::string(vendor), .status = vendor_state::unselected, .managed_count = 0});
    ++out.summary.unselected_vendors;
  }
  return out;
}

} // namespace

auto status(const options& opts) -> std::expected<status_result, status_error> {
  if (opts.planar_home.empty() || opts.home.empty() || opts.codex_home.empty()) {
    return std::unexpected(status_error::invalid_input);
  }
  if (opts.vendor && !is_vendor(*opts.vendor)) {
    return std::unexpected(status_error::invalid_vendor);
  }

  const std::filesystem::path manifest_path = std::filesystem::path(opts.planar_home) / "install-manifest.json";
  const std::string           bootstrap     = bootstrap_command(opts.planar_home);

  auto raw = try_read_file(manifest_path);
  if (!raw) {
    const std::filesystem::path stamp_path = std::filesystem::path(opts.planar_home) / ".planar-install";
    const bool                  legacy     = path_exists(stamp_path);
    return bootstrap_result(manifest_path.string(), bootstrap, legacy ? manifest_state::legacy : manifest_state::missing,
                            legacy ? "legacy ownership stamp exists but the versioned install manifest is missing"
                                   : "install manifest is missing",
                            opts.vendor);
  }

  auto parsed = json_dom::parse_json(*raw);
  if (!parsed) {
    return bootstrap_result(manifest_path.string(), bootstrap, manifest_state::invalid, "install manifest is invalid",
                            opts.vendor);
  }
  auto version = probe_version(*parsed);
  if (!version) {
    return bootstrap_result(manifest_path.string(), bootstrap, manifest_state::invalid, "install manifest is invalid",
                            opts.vendor);
  }
  if (*version != manifest_version) {
    return bootstrap_result(manifest_path.string(), bootstrap, manifest_state::unsupported,
                            "install manifest version is unsupported", opts.vendor);
  }
  auto manifest = parse_manifest_doc(*parsed);
  if (!manifest || !valid_manifest(*manifest)) {
    return bootstrap_result(manifest_path.string(), bootstrap, manifest_state::invalid, "install manifest is invalid",
                            opts.vendor);
  }

  std::vector<vendor_status>     vendors;
  std::vector<projection_status> projections;
  installed_surface::summary     summary;
  for (auto vendor : supported_vendors) {
    if (opts.vendor && *opts.vendor != vendor) {
      continue;
    }
    const bool  selected = std::ranges::find(manifest->vendors, vendor) != manifest->vendors.end();
    std::size_t count    = 0;
    if (selected) {
      for (auto const& row : manifest->projections) {
        if (row.vendor == vendor) {
          ++count;
        }
      }
    }
    vendors.push_back(vendor_status{.vendor        = std::string(vendor),
                                    .status        = selected ? vendor_state::selected : vendor_state::unselected,
                                    .managed_count = count});
    if (!selected) {
      ++summary.unselected_vendors;
    }
  }

  for (auto const& row : manifest->projections) {
    if (opts.vendor && *opts.vendor != row.vendor) {
      continue;
    }
    auto classified = classify_row(row, bootstrap);
    increment(summary, classified.status);
    projections.push_back(std::move(classified));
  }

  // Destination-only entries are visible, but absence from the manifest is
  // never treated as evidence that Planar owns or may repair them.
  for (auto const& vendor : vendors) {
    if (vendor.status == vendor_state::unselected) {
      continue;
    }
    discover_unmanaged(opts, *manifest, vendor.vendor, projections, summary);
  }
  std::ranges::sort(projections, less_projection);

  // There is no more per-name repair verb (plan 918 D5): any drift found
  // above (stale or missing) has exactly one recovery path now, the full
  // reinstall that re-shells scriptorium.
  std::optional<std::string> repair_command;
  if (summary.stale + summary.missing != 0) {
    repair_command = bootstrap;
  }

  return status_result{
      .manifest_status       = manifest_state::current,
      .manifest_version_seen = manifest->version,
      .build_id              = manifest->build_id,
      .install_mode          = manifest->install_mode,
      .manifest_path         = manifest_path.string(),
      .reason                = std::nullopt,
      .repair_command        = repair_command,
      .vendors               = std::move(vendors),
      .projections           = std::move(projections),
      .summary               = summary,
  };
}

} // namespace planar::installed_surface

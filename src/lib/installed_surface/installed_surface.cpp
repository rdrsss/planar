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

auto is_row_vendor(std::string_view value) -> bool {
  return is_vendor(value) || value == shared_vendor;
}

auto canonical_text(const std::filesystem::path& path) -> std::string {
  std::error_code ec;
  auto            out = std::filesystem::weakly_canonical(path, ec);
  return ec ? path.lexically_normal().string() : out.string();
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
  // Empty is a valid shape, not just 64-hex: projections carry no in-band
  // digest headers, so the install-manifest writer emits an empty string for
  // these fields. Manifest structural validation accepts that shape.
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
    if (!is_row_vendor(row.vendor)) {
      return false;
    }
    // `shared` names a root several vendors read; every other row vendor must
    // also be one the manifest selected.
    if (row.vendor != shared_vendor && std::ranges::find(manifest.vendors, row.vendor) == manifest.vendors.end()) {
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

/// @brief The raw nine-root table: paths relative to `$HOME` (or to
/// `$CODEX_HOME` for the Codex agents root).
struct root_spec {
  std::string_view                path;
  bool                            codex_relative;
  std::string_view                kind;
  bool                            directory_shape;
  bool                            derived;
  std::array<std::string_view, 4> vendors;
  std::size_t                     vendor_count;
};

constexpr std::array<root_spec, 9> k_roots{{
    {".claude/skills", false, "skill", true, false, {"claude"}, 1},
    {".agents/skills", false, "skill", true, false, {"codex", "copilot", "gemini", "opencode"}, 4},
    {".gemini/antigravity-cli/skills", false, "skill", true, false, {"antigravity"}, 1},
    {".claude/agents", false, "agent", false, false, {"claude"}, 1},
    {"agents", true, "agent", false, false, {"codex"}, 1},
    {".copilot/agents", false, "agent", false, false, {"copilot"}, 1},
    {".gemini/agents", false, "agent", false, false, {"gemini"}, 1},
    {".gemini/antigravity-cli/agents", false, "agent", false, false, {"antigravity"}, 1},
    {".config/opencode/agents", false, "agent", false, true, {"opencode"}, 1},
}};

auto is_dir(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::is_directory(path, ec) && !ec;
}

auto is_file(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) && !ec;
}

/// @brief The presence markers of `install.sh`'s `vendor_present`.
auto vendor_present(const options& opts, std::string_view vendor) -> bool {
  const std::filesystem::path home(opts.home);
  if (vendor == "claude") {
    return is_dir(home / ".claude");
  }
  if (vendor == "codex") {
    return opts.codex_home_set || is_dir(home / ".codex");
  }
  if (vendor == "copilot") {
    return is_dir(home / ".copilot");
  }
  if (vendor == "gemini") {
    return is_file(home / ".gemini" / "settings.json");
  }
  if (vendor == "antigravity") {
    return is_dir(home / ".gemini" / "antigravity-cli");
  }
  if (vendor == "opencode") {
    return is_dir(home / ".config" / "opencode");
  }
  return false;
}

/// @brief The root `installed_path` sits directly inside, or null.
auto root_of(const std::vector<installed_root>& roots, std::string_view installed_path) -> const installed_root* {
  const auto parent = canonical_text(std::filesystem::path(installed_path).parent_path());
  for (auto const& root : roots) {
    if (canonical_text(root.path) == parent) {
      return &root;
    }
  }
  return nullptr;
}

/// @brief Every regular file under `root` as relative path -> bytes. With
/// `reject_links`, a symlink anywhere in the tree makes the result empty
/// (`nullopt`) so a copy that holds links never reads as matching.
auto read_tree(const std::filesystem::path& root, bool reject_links) -> std::optional<std::map<std::string, std::string>> {
  std::map<std::string, std::string>            out;
  std::error_code                               ec;
  std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none, ec);
  if (ec) {
    return std::nullopt;
  }
  for (; it != std::filesystem::recursive_directory_iterator{}; it.increment(ec)) {
    if (ec) {
      return std::nullopt;
    }
    auto const&     entry = *it;
    std::error_code sec;
    if (entry.is_symlink(sec)) {
      if (reject_links) {
        return std::nullopt;
      }
      if (entry.is_directory(sec)) {
        continue;
      }
    }
    if (!entry.is_regular_file(sec) || sec) {
      continue;
    }
    auto bytes = try_read_file(entry.path());
    if (!bytes) {
      return std::nullopt;
    }
    out.emplace(entry.path().lexically_relative(root).generic_string(), std::move(*bytes));
  }
  return out;
}

auto classify_row(const manifest_row& row, std::string_view bootstrap, bool derived) -> projection_status {
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

  const bool staged_is_dir = is_dir(row.staged_path);
  auto       staged        = staged_is_dir ? std::optional<std::string>{std::string{}} : try_read_file(row.staged_path);
  if (staged && derived) {
    staged = derive_opencode(*staged);
  }
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
  if (!staged_is_dir && installed_type == std::filesystem::file_type::directory) {
    base.reason = "managed installed destination is a directory and cannot be replaced safely";
    return base;
  }

  if (row.install_kind == "link") {
    // A link to the staged authority matches by construction; a dangling one
    // is a missing install, and anything else is not the link the manifest
    // recorded.
    if (is_link && link_target.string() == row.staged_path) {
      base.status         = state::fresh;
      base.reason         = "staged projection and installed projection agree";
      base.repair_command = std::nullopt;
      return base;
    }
    if (!path_exists(row.installed_path) || (!staged_is_dir && !try_read_file(row.installed_path))) {
      base.status = state::missing;
      base.reason = "managed installed projection is missing";
      return base;
    }
    base.reason = "managed link target differs from the install manifest";
    return base;
  }

  bool same = false;
  if (staged_is_dir) {
    if (is_link || installed_type != std::filesystem::file_type::directory) {
      base.reason = "managed installed bytes differ from the staged projection";
      return base;
    }
    auto want = read_tree(row.staged_path, false);
    auto have = read_tree(row.installed_path, true);
    if (!want) {
      return base;
    }
    same = have && *want == *have;
  } else {
    auto installed = try_read_file(row.installed_path);
    if (!installed) {
      base.status = state::missing;
      base.reason = "managed installed projection is missing";
      return base;
    }
    same = !is_link && *staged == *installed;
  }
  if (!same) {
    base.reason = "managed installed bytes differ from the staged projection";
    return base;
  }

  base.status         = state::fresh;
  base.reason         = "staged projection and installed projection agree";
  base.repair_command = std::nullopt;
  return base;
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

/// @brief The vendor a root's rows are filed under: its only vendor, or
/// `shared` when several read it.
auto root_owner(const installed_root& root) -> std::string {
  return root.vendors.size() == 1 ? root.vendors.front() : std::string(shared_vendor);
}

/// @brief Whether a row of `row_vendor` in `root` passes the `--vendor` filter.
auto passes_filter(const options& opts, const installed_root* root, std::string_view row_vendor) -> bool {
  if (!opts.vendor) {
    return true;
  }
  if (row_vendor == *opts.vendor) {
    return true;
  }
  return row_vendor == shared_vendor && root != nullptr && std::ranges::find(root->vendors, *opts.vendor) != root->vendors.end();
}

/// @brief Planar's own namespace in a vendor root: the skill `planar` and the
/// agents `planar-<role>`. Entries outside it (a user's other skills, their
/// own agents) are never reported: a shared root holds other tools' files.
auto in_planar_namespace(std::string_view name) -> bool {
  return name == "planar" || name.starts_with("planar-");
}

void discover_unmanaged(const options& opts, const manifest_doc& manifest, const std::vector<installed_root>& roots,
                        std::vector<projection_status>& projections, installed_surface::summary& summary) {
  for (auto const& root : roots) {
    const bool selected = std::ranges::any_of(
        root.vendors, [&](auto const& v) { return std::ranges::find(manifest.vendors, v) != manifest.vendors.end(); });
    if (!root.present || !selected || !passes_filter(opts, &root, root_owner(root))) {
      continue;
    }
    std::error_code                     ec;
    std::filesystem::directory_iterator it(root.path, ec);
    if (ec) {
      continue;
    }
    for (auto const& entry : it) {
      auto const name = entry.path().filename().string();
      if (!in_planar_namespace(projection_name(name))) {
        continue;
      }
      const std::string installed = entry.path().string();
      if (manifest_owns(manifest, installed)) {
        continue;
      }
      if (root.directory_shape && !path_exists(entry.path() / "SKILL.md")) {
        continue;
      }
      projections.push_back(projection_status{
          .vendor         = root_owner(root),
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

auto installed_roots(const options& opts) -> std::vector<installed_root> {
  std::vector<installed_root> out;
  out.reserve(k_roots.size());
  for (auto const& spec : k_roots) {
    installed_root root;
    root.path            = ((spec.codex_relative ? std::filesystem::path(opts.codex_home) : std::filesystem::path(opts.home)) /
                            std::filesystem::path(spec.path))
                               .string();
    root.kind            = std::string(spec.kind);
    root.directory_shape = spec.directory_shape;
    root.derived         = spec.derived;
    // Vendors read in `supported_vendors` order whatever the table's order.
    for (auto vendor : supported_vendors) {
      if (std::ranges::find(spec.vendors.begin(), spec.vendors.begin() + static_cast<std::ptrdiff_t>(spec.vendor_count),
                            vendor) != spec.vendors.begin() + static_cast<std::ptrdiff_t>(spec.vendor_count)) {
        root.vendors.emplace_back(vendor);
      }
    }
    root.present = std::ranges::any_of(root.vendors, [&](auto const& v) { return vendor_present(opts, v); });
    out.push_back(std::move(root));
  }
  return out;
}

auto derive_opencode(std::string_view staged) -> std::optional<std::string> {
  // Mirrors ownership.sh's awk program record for record: `\n` terminates a
  // record, an unterminated last record is still a record, and every output
  // record ends in `\n`.
  std::vector<std::string_view> lines;
  while (!staged.empty()) {
    const auto nl = staged.find('\n');
    lines.push_back(staged.substr(0, nl));
    staged = nl == std::string_view::npos ? std::string_view{} : staged.substr(nl + 1);
  }
  if (lines.empty()) {
    return std::string{};
  }
  if (lines.front() != "---") {
    return std::nullopt;
  }
  std::string out;
  std::string desc;
  bool        in_frontmatter = true;
  for (std::size_t i = 1; i < lines.size(); ++i) {
    const auto line = lines[i];
    if (in_frontmatter) {
      if (line == "---") {
        if (desc.empty()) {
          return std::nullopt;
        }
        constexpr std::string_view k_lead = "-?:,[]{}#&*!|>%@`";
        const bool                 quoted = desc.front() == '"' || desc.front() == '\'';
        const bool risky = desc.find(": ") != std::string::npos || desc.find(" #") != std::string::npos || desc.back() == ':' ||
                           k_lead.find(desc.front()) != std::string_view::npos;
        if (!quoted && risky) {
          std::string escaped = "\"";
          for (char ch : desc) {
            if (ch == '\\' || ch == '"') {
              escaped += '\\';
            }
            escaped += ch;
          }
          escaped += '"';
          desc = std::move(escaped);
        }
        out += std::format("---\ndescription: {}\nmode: subagent\n---\n", desc);
        in_frontmatter = false;
      } else if (line.starts_with("description:")) {
        auto rest = line.substr(std::string_view("description:").size());
        rest.remove_prefix(std::min(rest.find_first_not_of(" \t"), rest.size()));
        desc = std::string(rest);
      }
      continue;
    }
    out += line;
    out += '\n';
  }
  if (in_frontmatter) {
    return std::nullopt;
  }
  return out;
}

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
  if (*version == 1) {
    return bootstrap_result(manifest_path.string(), bootstrap, manifest_state::legacy,
                            "install manifest predates the nine-root vendor layout (version 1); reinstall to rewrite it",
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

  const auto                     roots = installed_roots(opts);
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
    auto const* root = root_of(roots, row.installed_path);
    if (!passes_filter(opts, root, row.vendor)) {
      continue;
    }
    // A row in one of the nine roots counts only while a vendor that reads
    // that root is present; a vendor removed since the install leaves no row.
    if (root != nullptr && !root->present) {
      continue;
    }
    auto classified = classify_row(row, bootstrap, root != nullptr && root->derived);
    increment(summary, classified.status);
    projections.push_back(std::move(classified));
  }

  // Destination-only entries are visible, but absence from the manifest is
  // never treated as evidence that Planar owns or may repair them.
  discover_unmanaged(opts, *manifest, roots, projections, summary);
  std::ranges::sort(projections, less_projection);

  // There is no per-name repair verb: any drift found above (stale or
  // missing) has exactly one recovery path, the full reinstall.
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

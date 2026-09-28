/// @file core.cpp
/// @brief Implementation of `planar.tools.scriptorium`: source parsing, per-vendor projection, and the
///   `check`/`status` comparisons. See the module interface for the entry point's contract.

module;
#include <glaze/json.hpp>
#include <glaze/toml.hpp>
#include <glaze/yaml.hpp>
module planar.tools.scriptorium;
import std;

namespace planar::tools::scriptorium {
namespace detail {
namespace fs = std::filesystem;

/// @brief Per-vendor frontmatter overrides a source may carry under `vendor.<name>`.
struct vendor_extra {
  std::string argument_hint;       ///< Claude `argument-hint` / Codex `argument_hint`; empty emits none.
  std::string invocation_examples; ///< Example invocations, emitted only for profiles with `invocation` set.
  std::string model;               ///< Vendor-specific model override.
  std::string name;                ///< Name emitted in Codex frontmatter; empty falls back to the slug.
};
/// @brief The frontmatter block of one authored source file.
struct frontmatter {
  std::string                         slug;           ///< Stable artifact name; also the output file or directory name.
  std::string                         kind = "skill"; ///< `skill`, `agent`, or `doc`.
  std::string                         description;    ///< One-line description rendered into every projection.
  std::string                         origin;         ///< Provenance note; the only extra key a `doc` may carry.
  std::string                         model;          ///< Default model for the artifact.
  std::vector<std::string>            tools;          ///< Tools the artifact is granted.
  std::vector<std::string>            shared_notes;   ///< Notes rendered as a `## Notes` list in a skill projection.
  std::map<std::string, vendor_extra> vendor;         ///< Per-vendor overrides, keyed by profile name.
};
/// @brief The `sources` table of the configuration file.
struct config_sources {
  std::string skills; ///< Directory holding skill sources.
  std::string agents; ///< Directory holding agent and doc sources.
};
/// @brief A per-vendor configuration override.
struct config_override {
  std::string output_dir; ///< Replaces the profile's `skill_dir` when non-empty.
};
/// @brief The parsed configuration file.
struct config {
  config_sources                         sources;          ///< Where the authored sources live.
  std::vector<std::string>               vendors;          ///< Vendor profiles to render, by name.
  std::map<std::string, config_override> vendor_overrides; ///< Per-vendor overrides, keyed by profile name.
};
/// @brief One authored source file, split into frontmatter and body.
struct source {
  frontmatter meta; ///< Parsed and validated frontmatter.
  fs::path    path; ///< File it was read from; names it in diagnostics.
  std::string body; ///< Template body after the frontmatter block.
};
/// @brief One rendered output file, relative to the output root.
struct projection {
  fs::path    path;  ///< Output path relative to the output root.
  std::string bytes; ///< Exact expected file contents.
  std::string slug;  ///< Slug of the source it was rendered from.
  std::string kind;  ///< Kind of the source it was rendered from.
};
/// @brief A built-in vendor profile: where and how that vendor's surfaces are rendered.
struct profile {
  std::string name;               ///< Profile key, e.g. `claude` or `codex`.
  std::string title;              ///< Display name rendered as `VendorTitle`.
  std::string skill_dir;          ///< Skill output directory, relative to the output root.
  std::string agent_dir;          ///< Agent and doc output directory, relative to the output root.
  std::string invoke;             ///< Invocation pattern; `<slug>` is substituted.
  std::string install_path;       ///< Installed-path pattern; `<slug>` is substituted.
  bool        dir_layout = false; ///< Skills render as `<slug>/SKILL.md` rather than `<slug>.md`; docs are skipped.
  bool        invocation = false; ///< Emit `invocation_examples` into skill projections.
};
/// @brief One artifact's row in the `status` report.
struct status_row {
  std::string slug;              ///< Artifact slug.
  std::string kind;              ///< Artifact kind.
  bool        defined   = true;  ///< A source defines it; false for an orphaned output file.
  bool        rendered  = false; ///< Every expected projection is staged byte-for-byte.
  bool        installed = false; ///< Every expected projection is installed and matches its staged copy.
  bool        drifted   = false; ///< A staged or installed copy differs from what is expected.
  bool        orphaned  = false; ///< An output file exists that no source produces.
};
/// @brief The `status` report: one row per defined or orphaned artifact.
struct status_report {
  std::vector<status_row> artifacts; ///< Rows ordered by kind, then slug.
};
/// @brief The `check` report; any finding makes the verb exit 1.
struct check_report {
  std::vector<std::string> missing;      ///< Expected outputs that are absent.
  std::vector<std::string> changed;      ///< Expected outputs whose bytes differ.
  std::vector<std::string> unexpected;   ///< Files in an owned output directory that no source produces.
  int                      failures = 0; ///< Total number of findings.
};
/// @brief One entry of the installer's `install-manifest.json`.
struct manifest_row {
  std::string vendor;         ///< Vendor profile the file was installed for.
  std::string kind;           ///< Artifact kind; companion docs are recorded as `agent`.
  std::string name;           ///< Artifact slug.
  std::string staged_path;    ///< Absolute path of the staged copy.
  std::string installed_path; ///< Absolute path of the installed copy.
};
/// @brief The installer's manifest of installed projections.
struct install_manifest {
  std::vector<manifest_row> projections; ///< Every installed file.
};

/// @brief Read a whole file as bytes.
/// @param path File to read.
/// @return Its contents; throws `std::runtime_error` when it cannot be opened.
auto read_file(const fs::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
/// @brief Quote a string for YAML or TOML frontmatter, escaping backslash, quote, newline, tab and CR.
/// @param s Unquoted value.
/// @return The double-quoted, escaped value.
auto quote(std::string_view s) -> std::string {
  std::string out = "\"";
  for (char c : s) {
    if (c == '\\' || c == '"')
      out += '\\';
    if (c == '\n')
      out += "\\n";
    else if (c == '\t')
      out += "\\t";
    else if (c == '\r')
      out += "\\r";
    else
      out += c;
  }
  out += '"';
  return out;
}
/// @brief Render a YAML flow list, quoting any item that is not a plain `[A-Za-z0-9_.-]+` token.
/// @param items List items.
/// @return The `[a, b]` flow list.
auto yaml_list(const std::vector<std::string>& items) -> std::string {
  std::string out = "[";
  for (const auto& item : items) {
    if (out.size() > 1)
      out += ", ";
    bool plain = !item.empty() &&
                 std::ranges::all_of(item, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '-'; });
    out += plain ? item : quote(item);
  }
  return out + "]";
}
/// @brief Render a TOML array of quoted strings.
/// @param items Array items.
/// @return The `["a", "b"]` array.
auto toml_list(const std::vector<std::string>& items) -> std::string {
  std::string out = "[";
  for (const auto& item : items) {
    if (out.size() > 1)
      out += ", ";
    out += quote(item);
  }
  return out + "]";
}
/// @brief Drop one leading line break so a body joins its frontmatter without a blank line of its own.
/// @param s Body text.
/// @return The body without a single leading `\n` or `\r\n`.
auto seam(std::string s) -> std::string {
  if (s.starts_with("\r\n"))
    return s.substr(2);
  if (s.starts_with("\n"))
    return s.substr(1);
  return s;
}
/// @brief Whether a slug is non-empty and only `[a-z0-9-]`.
/// @param s Candidate slug.
/// @return True when it is a valid slug.
auto valid_slug(std::string_view s) -> bool {
  return !s.empty() &&
         std::ranges::all_of(s, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
/// @brief Whether a path is non-empty, relative, and free of `.` and `..` components.
/// @param p Candidate output path.
/// @return True when it cannot escape the output root.
auto safe_relative(const fs::path& p) -> bool {
  if (p.empty() || p.is_absolute())
    return false;
  for (const auto& part : p)
    if (part == ".." || part == ".")
      return false;
  return true;
}
/// @brief The built-in vendor profiles.
/// @return Profiles keyed by name: `claude`, `codex`, `copilot`, `gemini`.
auto builtins() -> std::map<std::string, profile> {
  return {
      {"claude",
       {"claude", "Claude", "commands/claude", "agents/claude", "/<slug>", "~/.claude/commands/<slug>.md", false, true}},
      {"codex", {"codex", "Codex", "skills/codex", "agents/codex", "<slug>", "~/.codex/skills/<slug>/SKILL.md", true, false}},
      {"copilot",
       {"copilot", "Copilot", "skills/copilot", "agents/copilot", "<slug>", "~/.copilot/prompts/<slug>.md", false, false}},
      {"gemini", {"gemini", "Gemini", "skills/gemini", "agents/gemini", "/<slug>", "~/.gemini/commands/<slug>.md", false, false}},
  };
}
/// @brief Load and validate the YAML configuration file.
/// @param path Configuration file.
/// @return The configuration; throws when it is unreadable, malformed, or names no sources or vendors.
auto load_config(const fs::path& path) -> config {
  config cfg;
  auto   raw = read_file(path);
  if (auto e = glz::read_yaml(cfg, raw); e)
    throw std::runtime_error(path.string() + ": " + glz::format_error(e, raw));
  if (cfg.sources.skills.empty() || cfg.sources.agents.empty() || cfg.vendors.empty())
    throw std::runtime_error(path.string() + ": missing sources or vendors");
  return cfg;
}
/// @brief Load one authored source and grade its frontmatter strictly.
///
/// Unknown, duplicate, and kind-inappropriate keys are refused, as is a legacy Go template action in the
/// body, so a typo cannot silently change a projection.
/// @param path Source file.
/// @return The parsed source; throws on any validation failure.
auto load_source(const fs::path& path) -> source {
  auto raw = read_file(path);
  if (!raw.starts_with("---\n"))
    throw std::runtime_error(path.string() + ": missing YAML frontmatter");
  auto close = raw.find("\n---", 4);
  if (close == std::string::npos)
    throw std::runtime_error(path.string() + ": unterminated YAML frontmatter");
  auto   yaml = raw.substr(4, close - 4);
  source src;
  src.path = path;
  if (auto e = glz::read_yaml(src.meta, yaml); e)
    throw std::runtime_error(path.string() + ": " + glz::format_error(e, yaml));
  if (!valid_slug(src.meta.slug) || src.meta.description.empty())
    throw std::runtime_error(path.string() + ": invalid slug or missing description");
  if (src.meta.kind != "skill" && src.meta.kind != "agent" && src.meta.kind != "doc")
    throw std::runtime_error(path.string() + ": invalid kind " + src.meta.kind);
  // Glaze's default unknown-key handling is permissive; explicitly grade the
  // source schema so a typo cannot silently alter a projection.
  const std::set<std::string> allowed = {"slug", "kind", "description", "origin", "model", "tools", "shared_notes", "vendor"};
  std::istringstream          lines(yaml);
  std::string                 line;
  std::set<std::string>       seen;
  while (std::getline(lines, line)) {
    if (line.empty() || line[0] == '#' || std::isspace(static_cast<unsigned char>(line[0])))
      continue;
    auto colon = line.find(':');
    if (colon == std::string::npos || !allowed.contains(line.substr(0, colon)))
      throw std::runtime_error(path.string() + ": unknown frontmatter key " + line);
    const auto key = line.substr(0, colon);
    if (!seen.insert(key).second)
      throw std::runtime_error(path.string() + ": duplicate frontmatter key " + key);
    if ((src.meta.kind == "doc" && key != "slug" && key != "kind" && key != "description" && key != "origin") ||
        (src.meta.kind == "skill" && key == "model"))
      throw std::runtime_error(path.string() + ": invalid frontmatter key for kind " + key);
  }
  if (src.meta.kind == "doc" &&
      (!src.meta.vendor.empty() || !src.meta.tools.empty() || !src.meta.shared_notes.empty() || !src.meta.model.empty()))
    throw std::runtime_error(path.string() + ": doc contains skill/agent fields");
  src.body = raw.substr(close + 5);
  if (src.body.find("{{.") != std::string::npos)
    throw std::runtime_error(path.string() + ": legacy Go template action");
  return src;
}
/// @brief Load every `.md` source under the given roots, in path order.
/// @param roots Source directories; each must exist.
/// @return The sources; throws on a missing root or a duplicate slug.
auto discover(const std::vector<fs::path>& roots) -> std::vector<source> {
  std::vector<fs::path> paths;
  for (const auto& root : roots) {
    if (!fs::is_directory(root))
      throw std::runtime_error("source directory missing: " + root.string());
    for (const auto& entry : fs::recursive_directory_iterator(root))
      if (entry.is_regular_file() && entry.path().extension() == ".md")
        paths.push_back(entry.path());
  }
  std::ranges::sort(paths);
  std::vector<source>   out;
  std::set<std::string> slugs;
  for (const auto& path : paths) {
    auto src = load_source(path);
    if (!slugs.insert(src.meta.slug).second)
      throw std::runtime_error(path.string() + ": duplicate slug " + src.meta.slug);
    out.push_back(std::move(src));
  }
  return out;
}
/// @brief Substitute every `{{ Name }}` action in one authored body.
///
/// The authored corpus is a flat string context and nothing else: no
/// conditionals, loops, includes or filters. An unknown name, a malformed
/// action or an unterminated one FAILS the render, because a silently
/// ignored action would stage a plausible-looking projection.
/// @param src Source whose body is rendered; its path names the diagnostic.
/// @param ctx Flat context; nested fields are dotted (`Vendor.name`).
/// @return The rendered body.
auto substitute(const source& src, const std::map<std::string, std::string>& ctx) -> std::string {
  const std::string_view body{src.body};
  auto        fail = [&src](const std::string& why) { throw std::runtime_error(src.path.string() + ": template: " + why); };
  std::string out;
  out.reserve(body.size());
  for (std::size_t at = 0; at < body.size();) {
    const auto open = body.find("{{", at);
    if (open == std::string_view::npos) {
      out += body.substr(at);
      break;
    }
    out += body.substr(at, open - at);
    const auto close = body.find("}}", open + 2);
    if (close == std::string_view::npos)
      fail("unterminated action");
    auto name = body.substr(open + 2, close - (open + 2));
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
      name.remove_prefix(1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
      name.remove_suffix(1);
    const auto shaped = !name.empty() && name.front() != '.' && name.back() != '.' && std::ranges::all_of(name, [](char c) {
      return (std::isalnum(static_cast<unsigned char>(c)) != 0) || c == '_' || c == '.';
    });
    if (!shaped)
      fail("invalid action \"" + std::string{name} + "\"");
    const auto it = ctx.find(std::string{name});
    if (it == ctx.end())
      fail("unknown variable \"" + std::string{name} + "\"");
    out += it->second;
    at = close + 2;
  }
  return out;
}
/// @brief Render a source body against its template context.
/// @param src Source to render.
/// @param p Vendor profile supplying the vendor variables, or null for a vendor-neutral doc.
/// @return The rendered body.
auto render_body(const source& src, const profile* p) -> std::string {
  std::map<std::string, std::string> ctx{{"Slug", src.meta.slug}, {"Description", src.meta.description}};
  if (p) {
    std::string invoke = p->invoke, install = p->install_path;
    auto        replace_slug = [&](std::string& x) {
      auto pos = x.find("<slug>");
      if (pos != std::string::npos)
        x.replace(pos, 6, src.meta.slug);
    };
    replace_slug(invoke);
    replace_slug(install);
    auto it                           = src.meta.vendor.find(p->name);
    auto extra                        = it == src.meta.vendor.end() ? vendor_extra{} : it->second;
    ctx["VendorName"]                 = p->name;
    ctx["VendorTitle"]                = p->title;
    ctx["Invoke"]                     = invoke;
    ctx["InstallPath"]                = install;
    ctx["Model"]                      = extra.model.empty() ? src.meta.model : extra.model;
    ctx["Vendor.argument_hint"]       = extra.argument_hint;
    ctx["Vendor.invocation_examples"] = extra.invocation_examples;
    ctx["Vendor.model"]               = extra.model;
    ctx["Vendor.name"]                = extra.name;
  }
  return substitute(src, ctx);
}
/// @brief Render one source for one vendor profile.
/// @param src Source to render.
/// @param p Vendor profile.
/// @return The projection, or unset when the profile does not carry this kind (a doc under `dir_layout`).
auto project(const source& src, const profile& p) -> std::optional<projection> {
  const auto& m     = src.meta;
  auto        it    = m.vendor.find(p.name);
  auto        extra = it == m.vendor.end() ? vendor_extra{} : it->second;
  if (m.kind == "doc") {
    if (p.dir_layout)
      return std::nullopt;
    return projection{fs::path(p.agent_dir) / (m.slug + ".md"), render_body(src, nullptr), m.slug, m.kind};
  }
  auto        body = render_body(src, &p);
  std::string out;
  if (m.kind == "skill") {
    out = "---\n";
    if (p.name == "codex")
      out += "name: " + quote(extra.name.empty() ? m.slug : extra.name) + "\n";
    out += "description: " + quote(m.description) + "\n";
    if (p.name == "claude") {
      if (!extra.argument_hint.empty())
        out += "argument-hint: " + quote(extra.argument_hint) + "\n";
      if (!m.tools.empty()) {
        std::string joined;
        for (const auto& tool : m.tools) {
          if (!joined.empty())
            joined += ",";
          joined += tool;
        }
        out += "allowed-tools: " + quote(joined) + "\n";
      }
      if (!extra.model.empty())
        out += "model: " + quote(extra.model) + "\n";
    }
    out += "---\n\n";
    if (p.invocation && !extra.invocation_examples.empty()) {
      auto examples = extra.invocation_examples;
      while (examples.ends_with("\n"))
        examples.pop_back();
      out += "## Invocations\n\n```\n" + examples + "\n```\n\n";
    }
    if (!m.shared_notes.empty()) {
      out += "## Notes\n\n";
      for (const auto& note : m.shared_notes)
        out += "- " + note + "\n";
      out += "\n";
    }
    out += seam(body);
    auto path = fs::path(p.skill_dir) / m.slug;
    if (p.dir_layout)
      path /= "SKILL.md";
    else
      path += ".md";
    return projection{path, out, m.slug, m.kind};
  }
  const auto name  = extra.name.empty() ? m.slug : extra.name;
  const auto model = extra.model.empty() ? m.model : extra.model;
  if (p.name == "codex") {
    out = "name = " + quote(name) + "\ndescription = " + quote(m.description) + "\n";
    if (!m.tools.empty())
      out += "tools = " + toml_list(m.tools) + "\n";
    if (!model.empty())
      out += "model = " + quote(model) + "\n";
    if (!extra.argument_hint.empty())
      out += "argument_hint = " + quote(extra.argument_hint) + "\n";
    if (!extra.invocation_examples.empty())
      out += "invocation_examples = " + quote(extra.invocation_examples) + "\n";
    out += "developer_instructions = " + quote(body) + "\n";
    std::map<std::string, glz::generic_i64> parsed;
    if (auto e = glz::read_toml(parsed, out); e)
      throw std::runtime_error(src.path.string() + ": invalid Codex TOML: " + glz::format_error(e, out));
    return projection{fs::path(p.agent_dir) / (m.slug + ".toml"), out, m.slug, m.kind};
  }
  out = "---\nname: " + quote(name) + "\ndescription: " + quote(m.description) + "\n";
  if (!m.tools.empty())
    out += "tools: " + yaml_list(m.tools) + "\n";
  if (!model.empty())
    out += "model: " + quote(model) + "\n";
  out += "---\n\n" + seam(body);
  return projection{fs::path(p.agent_dir) / (m.slug + ".md"), out, m.slug, m.kind};
}
/// @brief Every projection the sources produce across the selected profiles.
/// @param sources Loaded sources.
/// @param profiles Selected vendor profiles.
/// @return Projections sorted by path; throws on an unsafe or duplicate output path.
auto expected(const std::vector<source>& sources, const std::vector<profile>& profiles) -> std::vector<projection> {
  std::vector<projection> result;
  std::set<fs::path>      paths;
  for (const auto& src : sources)
    for (const auto& profile : profiles) {
      auto p = project(src, profile);
      if (!p)
        continue;
      if (!safe_relative(p->path) || !paths.insert(p->path).second)
        throw std::runtime_error("invalid or duplicate projection path: " + p->path.string());
      result.push_back(std::move(*p));
    }
  std::ranges::sort(result, {}, &projection::path);
  return result;
}
/// @brief Resolve a projection path under the output root, refusing a symlinked parent directory.
/// @param root Output root.
/// @param relative Projection path relative to the root.
/// @return The absolute staged path; throws when a parent component is a symlink.
auto staged_path(const fs::path& root, const fs::path& relative) -> fs::path {
  auto current = root;
  for (const auto& part : relative.parent_path()) {
    current /= part;
    if (fs::is_symlink(current))
      throw std::runtime_error("symlinked projection parent: " + current.string());
  }
  return root / relative;
}
/// @brief Write a file by renaming a fully written temporary over it, creating parent directories.
/// @param path Destination file.
/// @param bytes Exact contents.
void write_atomic(const fs::path& path, const std::string& bytes) {
  fs::create_directories(path.parent_path());
  auto tmp = path;
  tmp += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  {
    std::ofstream file(tmp, std::ios::binary);
    if (!file)
      throw std::runtime_error("cannot write " + tmp.string());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!file)
      throw std::runtime_error("cannot write " + tmp.string());
  }
  fs::rename(tmp, path);
}
/// @brief Serialize a report as JSON.
/// @param value Report to serialize.
/// @return The JSON text; throws when serialization fails.
auto as_json(const auto& value) -> std::string {
  std::string out;
  if (auto e = glz::write_json(value, out); e)
    throw std::runtime_error("JSON serialization failed");
  return out;
}
} // namespace detail
using namespace detail;

auto run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) -> int {
  if (args.empty() || args[0] == "--help" || args[0] == "help") {
    out << "scriptorium <render|check|status|version> --config <path> [--output-root <dir>] [--vendor <name>] [--json]\n";
    return 0;
  }
  if (args[0] == "version") {
    out << "scriptorium 1.0\n";
    return 0;
  }
  const auto verb = args[0];
  if (verb != "render" && verb != "check" && verb != "status") {
    err << "unknown command: " << verb << '\n';
    return 2;
  }
  if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h")) {
    out << "scriptorium " << verb << " --config <path> [--output-root <dir>] [--vendor <name>] [--json]"
        << (verb == "render" ? " [--source <dir>] [--dry-run]" : "") << (verb == "status" ? " [--install-manifest <path>]" : "")
        << '\n';
    return 0;
  }
  fs::path                 config_path = "scriptorium.yaml", output_root = fs::current_path(), source_override, manifest_path;
  std::vector<std::string> vendors;
  bool                     dry_run = false, json = false;
  for (size_t i = 1; i < args.size(); ++i) {
    auto arg = args[i];
    if (arg == "--config" || arg == "-config" || arg == "--output-root" || arg == "--source" || arg == "--vendor" ||
        arg == "--install-manifest") {
      if (++i == args.size()) {
        err << "missing value for " << arg << '\n';
        return 2;
      }
      if (arg == "--config" || arg == "-config")
        config_path = args[i];
      else if (arg == "--output-root")
        output_root = args[i];
      else if (arg == "--source")
        source_override = args[i];
      else if (arg == "--vendor")
        vendors.push_back(args[i]);
      else if (arg == "--install-manifest")
        manifest_path = args[i];
    } else if (arg == "--json" || arg == "-json")
      json = true;
    else if (arg == "--dry-run" && verb == "render")
      dry_run = true;
    else {
      err << "unknown option: " << arg << '\n';
      return 2;
    }
  }
  try {
    config_path = fs::absolute(config_path);
    output_root = fs::absolute(output_root);
    auto cfg    = load_config(config_path);
    if (vendors.empty())
      vendors = cfg.vendors;
    std::ranges::sort(vendors);
    vendors.erase(std::unique(vendors.begin(), vendors.end()), vendors.end());
    auto                 known = builtins();
    std::vector<profile> profiles;
    for (const auto& name : vendors) {
      if (!known.contains(name))
        throw std::runtime_error("unknown vendor: " + name);
      auto p = known.at(name);
      if (auto it = cfg.vendor_overrides.find(name); it != cfg.vendor_overrides.end() && !it->second.output_dir.empty())
        p.skill_dir = it->second.output_dir;
      if (!safe_relative(p.skill_dir) || !safe_relative(p.agent_dir))
        throw std::runtime_error("unsafe vendor output directory: " + name);
      profiles.push_back(std::move(p));
    }
    const auto base        = config_path.parent_path();
    auto       roots       = source_override.empty() ? std::vector<fs::path>{base / cfg.sources.skills, base / cfg.sources.agents}
                                                     : std::vector<fs::path>{source_override};
    auto       sources     = discover(roots);
    auto       projections = expected(sources, profiles);
    std::vector<std::string> skipped;
    for (const auto& src : sources)
      if (src.meta.kind == "doc")
        for (const auto& p : profiles)
          if (p.dir_layout)
            skipped.push_back(src.meta.slug + ":" + p.name);
    if (verb == "render") {
      for (const auto& p : projections) {
        auto path = staged_path(output_root, p.path);
        if (!dry_run)
          write_atomic(path, p.bytes);
        if (!json)
          out << p.path.string() << '\n';
      }
      if (json)
        out << as_json(std::map<std::string, std::vector<std::string>>{{"written",
                                                                        [&] {
                                                                          std::vector<std::string> v;
                                                                          for (const auto& p : projections)
                                                                            v.push_back(p.path.string());
                                                                          return v;
                                                                        }()},
                                                                       {"skipped", skipped},
                                                                       {"failed", {}}})
            << '\n';
      return 0;
    }
    std::map<std::pair<std::string, std::string>, status_row> rows;
    for (const auto& src : sources)
      rows[{src.meta.kind, src.meta.slug}] = {src.meta.slug, src.meta.kind, true, true, false, false, false};
    int                failures = 0;
    check_report       findings;
    std::set<fs::path> expected_paths;
    for (const auto& p : projections) {
      expected_paths.insert(p.path);
      auto path  = staged_path(output_root, p.path);
      bool equal = fs::is_regular_file(path) && read_file(path) == p.bytes;
      if (!equal) {
        ++failures;
        (fs::exists(path) ? findings.changed : findings.missing).push_back(p.path.string());
        auto& row    = rows[{p.kind, p.slug}];
        row.rendered = false;
        row.drifted  = fs::exists(path);
        if (verb == "check" && !json)
          out << (fs::exists(path) ? "changed: " : "missing: ") << p.path.string() << '\n';
      }
    }
    {
      std::map<fs::path, std::string> owned_dirs;
      for (const auto& p : profiles) {
        owned_dirs.emplace(p.skill_dir, "skill");
        owned_dirs.emplace(p.agent_dir, "agent");
      }
      for (const auto& [dir, kind] : owned_dirs) {
        const auto absolute = staged_path(output_root, dir / "placeholder").parent_path();
        if (!fs::is_directory(absolute))
          continue;
        for (const auto& entry : fs::recursive_directory_iterator(absolute)) {
          if (!entry.is_regular_file())
            continue;
          auto relative = fs::relative(entry.path(), output_root);
          if (!expected_paths.contains(relative)) {
            ++failures;
            findings.unexpected.push_back(relative.string());
            if (verb == "check" && !json)
              out << "unexpected: " << relative.string() << '\n';
            if (verb == "status") {
              auto slug = relative.parent_path() == dir ? relative.stem().string() : relative.parent_path().filename().string();
              rows[{kind, slug}] = {slug, kind, false, false, false, false, true};
            }
          }
        }
      }
    }
    if (verb == "status") {
      if (!manifest_path.empty()) {
        install_manifest manifest;
        auto             raw = read_file(manifest_path);
        if (auto e = glz::read_json(manifest, raw); e)
          throw std::runtime_error(manifest_path.string() + ": " + glz::format_error(e, raw));
        std::set<std::string>                              selected(vendors.begin(), vendors.end());
        std::map<std::pair<std::string, std::string>, int> wanted, good_count;
        for (const auto& p : projections)
          ++wanted[{p.kind, p.slug}];
        for (const auto& item : manifest.projections) {
          if (!selected.contains(item.vendor))
            continue;
          auto key = std::pair{item.kind, item.name};
          auto it  = rows.find(key);
          // Companion docs are copied through the installer's agent-file
          // lane and therefore carry kind=agent in its manifest.
          if (it == rows.end() && item.kind == "agent") {
            key = {"doc", item.name};
            it  = rows.find(key);
          }
          if (it == rows.end())
            continue;
          const fs::path staged = item.staged_path, installed = item.installed_path;
          const bool     good =
              fs::is_regular_file(staged) && fs::is_regular_file(installed) && read_file(staged) == read_file(installed);
          if (good)
            ++good_count[key];
          if (!good) {
            it->second.drifted = true;
            ++failures;
          }
        }
        for (auto& [key, row] : rows) {
          if (!row.defined || wanted[key] == 0)
            continue;
          row.installed = good_count[key] == wanted[key];
          if (!row.installed)
            ++failures;
        }
      }
      status_report report;
      for (const auto& [key, row] : rows)
        report.artifacts.push_back(row);
      if (json)
        out << as_json(report) << '\n';
      else
        for (const auto& row : report.artifacts)
          out << row.kind << ':' << row.slug << " rendered=" << row.rendered << '\n';
    } else if (json) {
      findings.failures = failures;
      out << as_json(findings) << '\n';
    }
    return failures ? 1 : 0;
  } catch (const std::exception& e) {
    err << "scriptorium: " << e.what() << '\n';
    return 2;
  }
}
} // namespace planar::tools::scriptorium

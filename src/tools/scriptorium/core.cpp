module;
#include <glaze/json.hpp>
#include <glaze/toml.hpp>
#include <glaze/yaml.hpp>
#include <inja/inja.hpp>
module planar.tools.scriptorium;
import std;

namespace planar::tools::scriptorium {
namespace detail {
namespace fs = std::filesystem;

struct vendor_extra {
  std::string argument_hint;
  std::string invocation_examples;
  std::string model;
  std::string name;
};
struct frontmatter {
  std::string                         slug;
  std::string                         kind = "skill";
  std::string                         description;
  std::string                         origin;
  std::string                         model;
  std::vector<std::string>            tools;
  std::vector<std::string>            shared_notes;
  std::map<std::string, vendor_extra> vendor;
};
struct config_sources {
  std::string skills;
  std::string agents;
};
struct config_override {
  std::string output_dir;
};
struct config {
  config_sources                         sources;
  std::vector<std::string>               vendors;
  std::map<std::string, config_override> vendor_overrides;
};
struct source {
  frontmatter meta;
  fs::path    path;
  std::string body;
};
struct projection {
  fs::path    path;
  std::string bytes;
  std::string slug;
  std::string kind;
};
struct profile {
  std::string name;
  std::string title;
  std::string skill_dir;
  std::string agent_dir;
  std::string invoke;
  std::string install_path;
  bool        dir_layout = false;
  bool        invocation = false;
};
struct status_row {
  std::string slug;
  std::string kind;
  bool        defined   = true;
  bool        rendered  = false;
  bool        installed = false;
  bool        drifted   = false;
  bool        orphaned  = false;
};
struct status_report {
  std::vector<status_row> artifacts;
};
struct check_report {
  std::vector<std::string> missing;
  std::vector<std::string> changed;
  std::vector<std::string> unexpected;
  int                      failures = 0;
};
struct manifest_row {
  std::string vendor;
  std::string kind;
  std::string name;
  std::string staged_path;
  std::string installed_path;
};
struct install_manifest {
  std::vector<manifest_row> projections;
};

auto read_file(const fs::path& path) -> std::string {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot read " + path.string());
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
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
auto toml_list(const std::vector<std::string>& items) -> std::string {
  std::string out = "[";
  for (const auto& item : items) {
    if (out.size() > 1)
      out += ", ";
    out += quote(item);
  }
  return out + "]";
}
auto seam(std::string s) -> std::string {
  if (s.starts_with("\r\n"))
    return s.substr(2);
  if (s.starts_with("\n"))
    return s.substr(1);
  return s;
}
auto valid_slug(std::string_view s) -> bool {
  return !s.empty() &&
         std::ranges::all_of(s, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
auto safe_relative(const fs::path& p) -> bool {
  if (p.empty() || p.is_absolute())
    return false;
  for (const auto& part : p)
    if (part == ".." || part == ".")
      return false;
  return true;
}
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
auto load_config(const fs::path& path) -> config {
  config cfg;
  auto   raw = read_file(path);
  if (auto e = glz::read_yaml(cfg, raw); e)
    throw std::runtime_error(path.string() + ": " + glz::format_error(e, raw));
  if (cfg.sources.skills.empty() || cfg.sources.agents.empty() || cfg.vendors.empty())
    throw std::runtime_error(path.string() + ": missing sources or vendors");
  return cfg;
}
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
auto render_body(const source& src, const profile* p) -> std::string {
  nlohmann::json ctx = {{"Slug", src.meta.slug}, {"Description", src.meta.description}};
  if (p) {
    std::string invoke = p->invoke, install = p->install_path;
    auto        replace_slug = [&](std::string& x) {
      auto pos = x.find("<slug>");
      if (pos != std::string::npos)
        x.replace(pos, 6, src.meta.slug);
    };
    replace_slug(invoke);
    replace_slug(install);
    auto it            = src.meta.vendor.find(p->name);
    auto extra         = it == src.meta.vendor.end() ? vendor_extra{} : it->second;
    ctx["VendorName"]  = p->name;
    ctx["VendorTitle"] = p->title;
    ctx["Invoke"]      = invoke;
    ctx["InstallPath"] = install;
    ctx["Model"]       = extra.model.empty() ? src.meta.model : extra.model;
    ctx["Vendor"]      = {{"argument_hint", extra.argument_hint},
                          {"invocation_examples", extra.invocation_examples},
                          {"model", extra.model},
                          {"name", extra.name}};
  }
  inja::Environment env;
  env.set_html_autoescape(false);
  env.set_line_statement("@@INJA_LINE@@");
  env.set_search_included_templates_in_files(false);
  try {
    return env.render(src.body, ctx);
  } catch (const std::exception& e) {
    throw std::runtime_error(src.path.string() + ": template: " + e.what());
  }
}
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
auto staged_path(const fs::path& root, const fs::path& relative) -> fs::path {
  auto current = root;
  for (const auto& part : relative.parent_path()) {
    current /= part;
    if (fs::is_symlink(current))
      throw std::runtime_error("symlinked projection parent: " + current.string());
  }
  return root / relative;
}
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

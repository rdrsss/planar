/// @file schema.cpp
/// @brief Implementation of `planar.cliapp.schema::schema_json`.

module planar.cliapp.schema;

import std;
import cli11;
import planar.cliapp.walk;
import planar.json_text;

namespace planar::cliapp {

namespace {

auto quote(std::string_view text) -> std::string {
  return json_text::json_string(text);
}

auto bool_text(bool value) -> std::string {
  return value ? "true" : "false";
}

auto string_array(std::span<std::string const> values) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += quote(values[i]);
  }
  out += "]";
  return out;
}

// The predecessor emitter (`planar.cli.schema`) had no `completion` field
// on its own flag/positional types and emitted this constant for every
// one. Reproduced verbatim: this emitter carries exactly as much
// information, no less.
constexpr std::string_view k_completion_none = R"({"kind":"none","values":[]})";

// Likewise: no per-node `doc` field existed to source this from, so the
// Zig `Doc{}` empty-default shape was hardcoded. Reproduced verbatim.
constexpr std::string_view k_docs_empty = R"({"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],)"
                                          R"("bugs":[],"authors":[],"homepage":"","license":"","copyright":"",)"
                                          R"("version":"","sourceUrl":""})";

/// @brief The declared value set of `opt`, when it carries one.
///
/// CLI11 renders a `CLI::IsMember` validator into the option's type name
/// as `:{a,b,c}` (verified against CLI11 2.7.2 — see this module's
/// interface header). Parsing it back is how a choice set survives the
/// swap; an option with any other type name has no declared set.
/// @param opt The option.
/// @return The choice values, or empty.
auto choices_of(const CLI::Option& opt) -> std::vector<std::string> {
  std::string const type_name = opt.get_type_name();
  auto const        open      = type_name.find('{');
  auto const        close     = type_name.rfind('}');
  if (open == std::string::npos || close == std::string::npos || close < open) {
    return {};
  }
  std::string_view         body{type_name.data() + open + 1, close - open - 1};
  std::vector<std::string> out;
  std::string              current;
  for (char const c : body) {
    if (c == ',') {
      out.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(c);
  }
  if (!current.empty()) {
    out.push_back(current);
  }
  return out;
}

/// @brief The catalog `kind` tag for `opt`, derived from CLI11's own
/// type-name string (see this module's interface header, divergence 2).
/// @param opt The option.
/// @return One of the etcli `Kind` tag names.
auto kind_of(const CLI::Option& opt) -> std::string {
  if (opt.get_expected_max() == 0) {
    return "bool";
  }
  if (!choices_of(opt).empty()) {
    return "choice";
  }
  std::string const type_name = opt.get_type_name();
  if (type_name.find("NUMBER") != std::string::npos || type_name.find("INT") != std::string::npos) {
    return "int";
  }
  if (type_name.find("FLOAT") != std::string::npos) {
    return "float";
  }
  return "string";
}

/// @brief The manual/help placeholder for a non-bool value.
/// @param opt The option.
/// @param kind The already-derived kind tag.
/// @return The placeholder text.
auto value_name_of(const CLI::Option& opt, std::string_view kind) -> std::string {
  if (kind == "bool" || kind == "choice") {
    return "";
  }
  if (kind == "int") {
    return "N";
  }
  (void)opt;
  return "VALUE";
}

auto aliases_of(const CLI::Option& opt) -> std::vector<std::string> {
  std::vector<std::string> out;
  auto const&              longs = opt.get_lnames();
  for (std::size_t i = 1; i < longs.size(); ++i) {
    out.push_back("--" + longs[i]);
  }
  return out;
}

/// @brief `source` is `"inherited"` for a flag collected from an
/// ancestor, `"local"` for one declared directly on the node.
auto render_flag(const CLI::Option& opt, std::string_view source) -> std::string {
  auto const  kind    = kind_of(opt);
  auto const  choices = choices_of(opt);
  auto const  aliases = aliases_of(opt);
  auto const& shorts  = opt.get_snames();

  std::string out = "{";
  out += "\"long\":" + quote(canonical_name(opt)) + ",";
  out += "\"aliases\":" + string_array(aliases) + ",";
  out += "\"hidden\":false,";
  out += "\"deprecated\":null,";
  out += "\"short\":";
  out += shorts.empty() ? "null" : quote(shorts.front());
  out += ",";
  out += "\"kind\":" + quote(kind) + ",";
  out += "\"choices\":" + string_array(choices) + ",";
  out += "\"list\":" + bool_text(opt.get_expected_max() > 1) + ",";
  out += "\"count\":false,";
  out += "\"required\":" + bool_text(opt.get_required()) + ",";
  out += "\"source\":" + quote(source) + ",";
  out += "\"valueName\":" + quote(value_name_of(opt, kind)) + ",";
  out += "\"default\":";
  auto const default_str = opt.get_default_str();
  out += default_str.empty() ? "null" : quote(default_str);
  out += ",";
  out += "\"description\":" + quote(opt.get_description()) + ",";
  out += "\"completion\":" + std::string(k_completion_none) + ",";
  out += "\"env\":null";
  out += "}";
  return out;
}

/// @brief Render one command's flag array: inherited first, then local,
/// with a flag that appears in BOTH emitted ONCE.
///
/// The dedupe is load-bearing (plan 996, task 6040). etcli inherits a
/// parent's flags into every child at parse time; CLI11 does not, so a
/// child that must accept a parent flag has to REDECLARE it — which put
/// the flag in `inherited_flags` and in `local_flags` both, and the
/// catalog listed it twice. No group in this tree had a flag-carrying
/// PARENT before `handoff`, so nothing had exercised the overlap and the
/// duplicate went unnoticed until `src/cmd/catalog_parity.hpp` compared
/// `planar handoff show` against the oracle.
///
/// INHERITED WINS the `source` label, because that is what the oracle
/// emits for a flag the child gets from its parent: the whole point of the
/// redeclaration is to reproduce inheritance CLI11 lacks, so labelling it
/// `local` would describe the workaround rather than the surface.
///
/// Deduping cannot mask a genuine double-declaration: CLI11 throws
/// `OptionAlreadyAdded` at tree-build time for two options with the same
/// long name on ONE node, so an overlap can only ever be parent-vs-child.
/// @param root The root app, for the parent-chain walk.
/// @param node The command being rendered.
/// @param path The command's root-relative path.
/// @return The JSON array of flag objects.
auto render_flags(const CLI::App& root, const CLI::App& node, std::span<std::string const> path) -> std::string {
  std::set<std::string, std::less<>> seen;
  std::string                        out   = "[";
  bool                               first = true;

  auto emit = [&](const CLI::Option& opt, std::string_view source) {
    auto name = canonical_name(opt);
    if (!seen.insert(std::move(name)).second) {
      return;
    }
    if (!first) {
      out += ",";
    }
    first = false;
    out += render_flag(opt, source);
  };

  for (const CLI::Option* opt : inherited_flags(root, path)) {
    emit(*opt, "inherited");
  }
  for (const CLI::Option* opt : local_flags(node)) {
    emit(*opt, "local");
  }
  out += "]";
  return out;
}

auto render_positionals(const CLI::App& node) -> std::string {
  std::string out   = "[";
  bool        first = true;
  for (const CLI::Option* opt : local_positionals(node)) {
    if (!first) {
      out += ",";
    }
    first           = false;
    auto const kind = kind_of(*opt);
    out += "{";
    out += "\"name\":" + quote(canonical_name(*opt)) + ",";
    out += "\"kind\":" + quote(kind) + ",";
    out += "\"required\":" + bool_text(opt->get_required()) + ",";
    out += "\"default\":";
    auto const default_str = opt->get_default_str();
    out += default_str.empty() ? "null" : quote(default_str);
    out += ",";
    out += "\"description\":" + quote(opt->get_description()) + ",";
    out += "\"completion\":" + std::string(k_completion_none);
    out += "}";
  }
  out += "]";
  return out;
}

auto render_subcommands(const CLI::App& node) -> std::string {
  std::vector<std::string> names;
  for (const CLI::App* child : children(node)) {
    names.push_back(child->get_name());
  }
  return string_array(names);
}

auto command_path(const CLI::App& root, std::span<std::string const> path) -> std::string {
  std::string out = root.get_name();
  for (auto const& segment : path) {
    out += " " + segment;
  }
  return out;
}

auto render_command(const CLI::App& root, const CLI::App& node, std::span<std::string const> path) -> std::string {
  std::vector<std::string> const path_vec(path.begin(), path.end());
  auto const                     description = node.get_description();

  std::string out = "{";
  out += "\"name\":" + quote(node.get_name()) + ",";
  out += "\"aliases\":" + string_array(node.get_aliases()) + ",";
  out += "\"hidden\":false,";
  out += "\"deprecated\":null,";
  out += "\"path\":" + string_array(path_vec) + ",";
  out += "\"command\":" + quote(command_path(root, path)) + ",";
  // One description string on `CLI::App` where `cli::cmd` carried two.
  // See this module's interface header, divergence 1.
  out += "\"summary\":" + quote(description) + ",";
  out += "\"description\":" + quote(description) + ",";
  out += "\"subcommands\":" + render_subcommands(node) + ",";
  out += "\"flags\":" + render_flags(root, node, path) + ",";
  out += "\"flagGroups\":[],";
  out += "\"positionals\":" + render_positionals(node) + ",";
  out += "\"docs\":" + std::string(k_docs_empty);
  out += "}";
  return out;
}

} // namespace

auto schema_json(const CLI::App& root) -> std::string {
  std::string out = "{";
  out += "\"schemaVersion\":1,";
  out += "\"layout\":\"flat\",";
  out += "\"root\":" + quote(root.get_name()) + ",";
  out += "\"commands\":[";
  out += render_command(root, root, {});
  for (auto const& node : all_nodes(root)) {
    out += "," + render_command(root, *node.node, node.path);
  }
  out += "]";
  out += "}";
  return out;
}

} // namespace planar::cliapp

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

/// @brief The JSON literal for an option's declared default.
///
/// TYPED, not blanket-quoted (plan 996, task 6065). Measured against the
/// oracle's three catalogs: a `bool` flag reports `false` (never `null` —
/// a bool flag's default IS false, and CLI11 simply has no default STRING
/// to read for one), an `int` flag reports a bare NUMBER, and everything
/// else reports a quoted string. The predecessor emitted `null` for every
/// bool and a quoted string for every int, which disagreed with the oracle
/// on 341 and 11 flags respectively across the three binaries.
///
/// Four oracle flags declare an
/// EMPTY-STRING default. Task 6130 closed this by supplying them as a
/// side-table to `schema_json` — see its three-argument overload. The
/// analysis below is retained because it is still why `default_literal`
/// alone cannot answer: (`planar workbench edit --editor`, `planar workflow
/// run --args`, and two others) and report `""` where this reports `null`.
/// CLI11 exposes one accessor, `get_default_str()`, returning `""` for both
/// "no default" and "a default that is the empty string"; the two are not
/// distinguishable from the tree, and mapping empty to `""` would make
/// every one of the ~500 flags with NO default report a default instead.
/// @param opt The option.
/// @param kind The already-derived kind tag.
/// @return The JSON literal.
auto default_literal(const CLI::Option& opt, std::string_view kind) -> std::string {
  auto const default_str = opt.get_default_str();
  if (kind == "bool") {
    // `false` unless the tree declared otherwise. Two oracle flags do —
    // `planar task add --editor` and `planar artifact add --editor` both
    // default to `true` (the verb opens $EDITOR unless told not to) — and
    // a blanket `false` would misreport them.
    return default_str == "true" ? "true" : "false";
  }
  if (default_str.empty()) {
    return "null";
  }
  if (kind == "int") {
    long long   value  = 0;
    auto const* first  = default_str.data();
    auto const* last   = first + default_str.size();
    auto const  parsed = std::from_chars(first, last, value);
    if (parsed.ec == std::errc{} && parsed.ptr == last) {
      return std::to_string(value);
    }
  }
  return quote(default_str);
}

/// @brief The option's declared aliases — its long names after the first,
/// MINUS any that is a CLI11 negation name.
///
/// The subtraction is load-bearing (plan 996, task 6138). Every bool flag
/// is declared through `planar.cliapp.surface::add_bool_flag`, which files
/// `--no-X` as a `!`-prefixed name so the parser accepts the negation the
/// oracle synthesizes. CLI11 stores such a name in BOTH `fnames_` and
/// `lnames_`, so reading `lnames_` alone would report `--no-json` as an
/// alias of `--json` on all 341 bool flags — a catalog the oracle's does
/// not match, since the oracle declares no negations at all. `fnames_` is
/// the exact set to drop: nothing but a negation ever lands in it.
/// @param opt The option.
/// @return The alias long names, `--` included.
auto aliases_of(const CLI::Option& opt) -> std::vector<std::string> {
  std::vector<std::string> out;
  auto const&              longs   = opt.get_lnames();
  auto const&              negated = opt.get_fnames();
  for (std::size_t i = 1; i < longs.size(); ++i) {
    if (std::ranges::find(negated, longs[i]) != negated.end()) {
      continue;
    }
    out.push_back("--" + longs[i]);
  }
  return out;
}

/// @brief The flag long names declared with an empty-string default on
/// `command`.
/// @param table The `(command path, flag long name)` side-table.
/// @param command The full command path being rendered.
/// @return The flag names, as a lookup set.
auto empty_defaults_for(std::span<std::pair<std::string_view, std::string_view> const> table, std::string_view command)
    -> std::set<std::string, std::less<>> {
  std::set<std::string, std::less<>> out;
  for (auto const& [path, flag] : table) {
    if (path == command) {
      out.emplace(flag);
    }
  }
  return out;
}

/// @brief `source` is `"inherited"` for a flag collected from an
/// ancestor, `"local"` for one declared directly on the node.
auto render_flag(const CLI::Option& opt, std::string_view source, bool empty_default) -> std::string {
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
  out += "\"default\":" + (empty_default ? std::string{"\"\""} : default_literal(opt, kind)) + ",";
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
auto render_flags(const CLI::App& root, const CLI::App& node, std::span<std::string const> path,
                  std::set<std::string, std::less<>> const& empty_defaults) -> std::string {
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
    out += render_flag(opt, source, empty_defaults.contains(canonical_name(opt)));
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

/// @brief The supplied one-line summary for `command`, or the node's own
/// description when the table does not carry one.
/// @param summaries The `(command path, summary)` table.
/// @param command The full command path.
/// @param description The node's description, used as the fallback.
/// @return The summary text.
auto summary_for(std::span<std::pair<std::string_view, std::string_view> const> summaries, std::string_view command,
                 std::string const& description) -> std::string_view {
  for (auto const& [key, text] : summaries) {
    if (key == command) {
      return text;
    }
  }
  return description;
}

auto render_command(const CLI::App& root, const CLI::App& node, std::span<std::string const> path,
                    std::span<std::pair<std::string_view, std::string_view> const> summaries,
                    std::span<std::pair<std::string_view, std::string_view> const> empty_string_defaults) -> std::string {
  std::vector<std::string> const path_vec(path.begin(), path.end());
  auto const                     description = node.get_description();
  auto const                     command     = command_path(root, path);

  std::string out = "{";
  out += "\"name\":" + quote(node.get_name()) + ",";
  out += "\"aliases\":" + string_array(node.get_aliases()) + ",";
  out += "\"hidden\":false,";
  out += "\"deprecated\":null,";
  out += "\"path\":" + string_array(path_vec) + ",";
  out += "\"command\":" + quote(command) + ",";
  // One description string on `CLI::App` where `cli::cmd` carried two, so
  // the summary is supplied out of band and falls back to the description.
  // See this module's interface header, divergence 1.
  out += "\"summary\":" + quote(summary_for(summaries, command, description)) + ",";
  out += "\"description\":" + quote(description) + ",";
  out += "\"subcommands\":" + render_subcommands(node) + ",";
  out += "\"flags\":" + render_flags(root, node, path, empty_defaults_for(empty_string_defaults, command)) + ",";
  out += "\"flagGroups\":[],";
  out += "\"positionals\":" + render_positionals(node) + ",";
  out += "\"docs\":" + std::string(k_docs_empty);
  out += "}";
  return out;
}

} // namespace

auto schema_json(const CLI::App& root) -> std::string {
  return schema_json(root, {}, {});
}

auto schema_json(const CLI::App& root, std::span<std::pair<std::string_view, std::string_view> const> summaries) -> std::string {
  return schema_json(root, summaries, {});
}

auto schema_json(const CLI::App& root, std::span<std::pair<std::string_view, std::string_view> const> summaries,
                 std::span<std::pair<std::string_view, std::string_view> const> empty_string_defaults) -> std::string {
  std::string out = "{";
  out += "\"schemaVersion\":1,";
  out += "\"layout\":\"flat\",";
  out += "\"root\":" + quote(root.get_name()) + ",";
  out += "\"commands\":[";
  out += render_command(root, root, {}, summaries, empty_string_defaults);
  for (auto const& node : all_nodes(root)) {
    out += "," + render_command(root, *node.node, node.path, summaries, empty_string_defaults);
  }
  out += "]";
  out += "}";
  return out;
}

} // namespace planar::cliapp

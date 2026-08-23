/// @file schema.cpp
/// @brief Implementation of `planar.cli.schema::schema_json`.
module;

module planar.cli.schema;

import std;
import planar.cli.flag;
import planar.cli.cmd;

namespace planar::cli {

namespace {

auto json_string(std::string_view text) -> std::string {
  std::string out = "\"";
  for (unsigned char c : text) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    case 0x08:
      out += "\\b";
      break;
    case 0x0c:
      out += "\\f";
      break;
    default:
      if (c <= 0x1f) {
        out += std::format("\\u{:04x}", c);
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  out += "\"";
  return out;
}

auto bool_text(bool b) -> std::string {
  return b ? "true" : "false";
}

auto string_array(std::vector<std::string> const& values) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += json_string(values[i]);
  }
  out += "]";
  return out;
}

auto visible_cmd(cmd const& c) -> bool {
  return !c.hidden;
}

auto visible_flag(flag const& f) -> bool {
  return !f.hidden;
}

// etcli-zig's Kind tag names (zig/vendor/etcli-zig/src/cli/flag.zig: `pub const Kind
// = enum { bool, string, int, float, duration, path, choice };`).
auto kind_name(kind k) -> std::string_view {
  switch (k) {
  case kind::boolean:
    return "bool";
  case kind::string:
    return "string";
  case kind::integer:
    return "int";
  case kind::floating:
    return "float";
  case kind::duration:
    return "duration";
  case kind::path:
    return "path";
  case kind::choice:
    return "choice";
  }
  return "string";
}

auto fallback_value_name(kind k) -> std::string_view {
  switch (k) {
  case kind::boolean:
    return "";
  case kind::string:
    return "VALUE";
  case kind::integer:
    return "N";
  case kind::floating:
    return "X";
  case kind::duration:
    return "DURATION";
  case kind::path:
    return "PATH";
  case kind::choice:
    return "";
  }
  return "VALUE";
}

// This port carries no `completion` field on `flag`/`positional` — always
// the Zig default-empty shape (see schema.cppm's file comment).
constexpr std::string_view k_completion_none = R"({"kind":"none","values":[]})";

// This port carries no per-node `doc` field on `cmd` — always the Zig
// `Doc{}` empty-default shape (see schema.cppm's file comment).
constexpr std::string_view k_docs_empty = R"({"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],)"
                                          R"("bugs":[],"authors":[],"homepage":"","license":"","copyright":"",)"
                                          R"("version":"","sourceUrl":""})";

auto render_value(value const& v) -> std::string {
  return std::visit(
      [](auto const& alt) -> std::string {
        using T = std::decay_t<decltype(alt)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          return bool_text(alt);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return std::format("{}", alt);
        } else if constexpr (std::is_same_v<T, double>) {
          return std::format("{}", alt);
        } else if constexpr (std::is_same_v<T, std::string>) {
          return json_string(alt);
        } else {
          // vector<std::string> — not a real Default shape (list flags take
          // no default per flag.cppm's doc comment); rendered defensively
          // as a JSON array so a future misuse fails loudly in a diff
          // rather than silently mis-typing the catalog.
          return string_array(alt);
        }
      },
      v);
}

auto render_default(std::optional<value> const& default_value) -> std::string {
  if (!default_value.has_value()) {
    return "null";
  }
  return render_value(*default_value);
}

auto render_flag_group(flag_group const& group) -> std::string {
  std::string mode_name;
  switch (group.mode) {
  case flag_group_mode::mutually_exclusive:
    mode_name = "mutually_exclusive";
    break;
  case flag_group_mode::required_one:
    mode_name = "required_one";
    break;
  case flag_group_mode::required_exactly_one:
    mode_name = "required_exactly_one";
    break;
  }
  std::string out = "{";
  out += "\"name\":" + json_string(group.name) + ",";
  out += "\"mode\":" + json_string(mode_name) + ",";
  out += "\"flags\":" + string_array(group.flags) + ",";
  out += "\"description\":" + json_string(group.desc);
  out += "}";
  return out;
}

// `source` is "inherited" for flags collected from an ancestor's own
// `flags`, "local" for flags declared directly on `node`.
auto render_flag(flag const& f, std::string_view source) -> std::string {
  std::string out = "{";
  out += "\"long\":" + json_string(f.long_name) + ",";
  out += "\"aliases\":" + string_array(f.aliases) + ",";
  out += "\"hidden\":" + bool_text(f.hidden) + ",";
  out += "\"deprecated\":null,";
  out += "\"short\":";
  if (f.short_name.has_value()) {
    out += json_string(std::string(1, *f.short_name));
  } else {
    out += "null";
  }
  out += ",";
  out += "\"kind\":" + json_string(kind_name(f.value_kind)) + ",";
  out += "\"choices\":" + string_array(f.choices) + ",";
  out += "\"list\":" + bool_text(f.list) + ",";
  out += "\"count\":" + bool_text(f.count) + ",";
  out += "\"required\":" + bool_text(f.required) + ",";
  out += "\"source\":" + json_string(source) + ",";
  out += "\"valueName\":";
  if (f.value_name.has_value()) {
    out += json_string(*f.value_name);
  } else {
    out += json_string(fallback_value_name(f.value_kind));
  }
  out += ",";
  out += "\"default\":" + render_default(f.default_value) + ",";
  out += "\"description\":" + json_string(f.desc) + ",";
  out += "\"completion\":" + std::string(k_completion_none);
  out += ",\"env\":";
  if (f.env.has_value()) {
    out += json_string(*f.env) + ",\"envBehavior\":\"cli-run-fallback\"";
  } else {
    out += "null";
  }
  out += "}";
  return out;
}

auto render_flags(cmd const& root, cmd const& node, std::span<std::string const> path) -> std::string {
  auto const  inherited = collect_inherited_flags(root, path);
  std::string out       = "[";
  bool        first     = true;
  for (auto const& f : inherited) {
    if (!visible_flag(f)) {
      continue;
    }
    if (!first) {
      out += ",";
    }
    first = false;
    out += render_flag(f, "inherited");
  }
  for (auto const& f : node.flags) {
    if (!visible_flag(f)) {
      continue;
    }
    if (!first) {
      out += ",";
    }
    first = false;
    out += render_flag(f, "local");
  }
  out += "]";
  return out;
}

auto flag_by_long(std::vector<flag> const& flags, std::string const& long_name) -> flag const* {
  for (auto const& f : flags) {
    if (f.long_name == long_name) {
      return &f;
    }
  }
  return nullptr;
}

auto visible_flag_group(flag_group const& group, std::vector<flag> const& visible_flags) -> bool {
  if (group.flags.empty()) {
    return false;
  }
  for (auto const& member : group.flags) {
    flag const* f = flag_by_long(visible_flags, member);
    if (f == nullptr || !visible_flag(*f)) {
      return false;
    }
  }
  return true;
}

auto render_flag_groups(cmd const& root, cmd const& node, std::span<std::string const> path) -> std::string {
  auto const        inherited = collect_inherited_flags(root, path);
  std::vector<flag> visible_flags(inherited);
  visible_flags.insert(visible_flags.end(), node.flags.begin(), node.flags.end());
  std::string out   = "[";
  bool        first = true;
  for (auto const& group : node.flag_groups) {
    if (!visible_flag_group(group, visible_flags)) {
      continue;
    }
    if (!first) {
      out += ",";
    }
    first = false;
    out += render_flag_group(group);
  }
  out += "]";
  return out;
}

auto render_positionals(std::vector<positional> const& positionals) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < positionals.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    auto const& p = positionals[i];
    out += "{";
    out += "\"name\":" + json_string(p.name) + ",";
    out += "\"kind\":" + json_string(kind_name(p.value_kind)) + ",";
    out += "\"required\":" + bool_text(p.required) + ",";
    out += "\"default\":" + render_default(p.default_value) + ",";
    out += "\"description\":" + json_string(p.desc) + ",";
    out += "\"completion\":" + std::string(k_completion_none);
    out += "}";
  }
  out += "]";
  return out;
}

auto render_subcommands(std::vector<cmd> const& cmds) -> std::string {
  std::string out   = "[";
  bool        first = true;
  for (auto const& child : cmds) {
    if (!visible_cmd(child)) {
      continue;
    }
    if (!first) {
      out += ",";
    }
    first = false;
    out += json_string(child.name);
  }
  out += "]";
  return out;
}

auto command_path(cmd const& root, std::span<std::string const> path) -> std::string {
  std::string out = root.name;
  for (auto const& seg : path) {
    out += " " + seg;
  }
  return out;
}

auto render_command(cmd const& root, cmd const& node, std::span<std::string const> path) -> std::string {
  std::string out = "{";
  out += "\"name\":" + json_string(node.name) + ",";
  out += "\"aliases\":" + string_array(node.aliases) + ",";
  out += "\"hidden\":" + bool_text(node.hidden) + ",";
  out += "\"deprecated\":null,";
  out += "\"path\":" + string_array(std::vector<std::string>(path.begin(), path.end())) + ",";
  out += "\"command\":" + json_string(command_path(root, path)) + ",";
  out += "\"summary\":" + json_string(node.desc) + ",";
  out += "\"description\":" + json_string(!node.long_desc.empty() ? node.long_desc : node.desc) + ",";
  out += "\"subcommands\":" + render_subcommands(node.cmds) + ",";
  out += "\"flags\":" + render_flags(root, node, path) + ",";
  out += "\"flagGroups\":" + render_flag_groups(root, node, path) + ",";
  out += "\"positionals\":" + render_positionals(node.positionals) + ",";
  out += "\"docs\":" + std::string(k_docs_empty);
  out += "}";
  return out;
}

/// @brief Depth-first pre-order walk emitting every VISIBLE descendant of
/// `node`, PRUNING an entire subtree the moment a hidden child is reached —
/// matches zig/vendor/etcli-zig/src/cli/schema.zig's `renderDescendantCommands`
/// (`if (!visibleCmd(child, options)) continue;` inside the per-child
/// loop: the loop `continue`s past the child WITHOUT ever recursing into
/// it, so nothing beneath a hidden node is walked at all, let alone
/// rendered).
///
/// B3 (M2 boundary review, plan 996 task 6066): the code this replaced
/// called `planar.cli.cmd::all_nodes` (cmd.cppm), which flattens the WHOLE
/// tree unconditionally (it recurses into every child regardless of
/// visibility — it has other callers, e.g. completion.cpp, that need the
/// full tree including hidden nodes), and then filtered `if
/// (!visible_cmd(*node)) continue;` per FLATTENED node. That filter only
/// excludes a hidden node ITSELF; a visible child of a hidden parent still
/// appears in `all_nodes`'s flattened list and passes the per-node
/// `visible_cmd` check, so it was wrongly emitted — not a subset of the
/// Zig catalog, a genuine divergence. This is therefore a second,
/// schema-specific walk rather than a change to `all_nodes`'s contract.
auto render_descendant_commands(cmd const& root, cmd const& node, std::vector<std::string> const& path) -> std::string {
  std::string out;
  for (auto const& child : node.cmds) {
    if (!visible_cmd(child)) {
      continue; // Prune: do not render this child, and do not recurse into it.
    }
    auto child_path = path;
    child_path.push_back(child.name);
    out += "," + render_command(root, child, child_path);
    out += render_descendant_commands(root, child, child_path);
  }
  return out;
}

} // namespace

auto schema_json(cmd const& root) -> std::string {
  std::string out = "{";
  out += "\"schemaVersion\":1,";
  out += "\"layout\":\"flat\",";
  out += "\"root\":" + json_string(root.name) + ",";
  out += "\"commands\":[";
  out += render_command(root, root, {});
  out += render_descendant_commands(root, root, {});
  out += "]";
  out += "}";
  return out;
}

} // namespace planar::cli

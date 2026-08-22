/// @file help.cpp
/// @brief Implementation of `planar.cli.help::render_help`.
module;

module planar.cli.help;

import std;
import planar.cli.flag;
import planar.cli.cmd;

namespace planar::cli {

namespace {

auto pad_to(std::string_view s, std::size_t width) -> std::string {
  if (s.size() >= width) {
    return "  ";
  }
  return std::string(width - s.size(), ' ');
}

auto visible_cmd(cmd const& c) -> bool {
  return !c.hidden;
}

auto visible_flag(flag const& f) -> bool {
  return !f.hidden;
}

auto has_visible_commands(std::vector<cmd> const& cmds) -> bool {
  return std::ranges::any_of(cmds, visible_cmd);
}

auto has_visible_flags(std::vector<flag> const& flags) -> bool {
  return std::ranges::any_of(flags, visible_flag);
}

auto has_visible_env(std::vector<flag> const& flags) -> bool {
  return std::ranges::any_of(flags, [](flag const& f) { return visible_flag(f) && f.env.has_value(); });
}

auto flag_by_long(std::vector<flag> const& flags, std::string_view long_name) -> const flag* {
  for (auto const& f : flags) {
    if (f.long_name == long_name) {
      return &f;
    }
  }
  return nullptr;
}

auto visible_flag_group(flag_group const& group, std::vector<flag> const& flags) -> bool {
  if (group.flags.empty()) {
    return false;
  }
  for (auto const& member : group.flags) {
    const flag* f = flag_by_long(flags, member);
    if (f == nullptr || !visible_flag(*f)) {
      return false;
    }
  }
  return true;
}

auto has_visible_flag_groups(std::vector<flag_group> const& groups, std::vector<flag> const& flags) -> bool {
  return std::ranges::any_of(groups, [&](flag_group const& g) { return visible_flag_group(g, flags); });
}

auto cmd_label(cmd const& c) -> std::string {
  std::string out = c.name;
  for (auto const& alias : c.aliases) {
    out += ", ";
    out += alias;
  }
  return out;
}

auto render_path(std::span<std::string const> path, std::string_view leaf_name) -> std::string {
  if (path.empty()) {
    return std::string(leaf_name);
  }
  std::string out(path[0]);
  for (std::size_t i = 1; i < path.size(); ++i) {
    out += " ";
    out += path[i];
  }
  return out;
}

auto kind_label(kind k) -> std::string_view {
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

auto join_choices(std::vector<std::string> const& choices) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < choices.size(); ++i) {
    if (i > 0) {
      out += "|";
    }
    out += choices[i];
  }
  return out;
}

auto flag_kind_label(flag const& f) -> std::string {
  if (f.count) {
    return "count";
  }
  if (f.value_kind == kind::choice) {
    return join_choices(f.choices);
  }
  return std::string(kind_label(f.value_kind));
}

auto render_default(value const& d) -> std::string {
  if (auto const* b = std::get_if<bool>(&d)) {
    return *b ? "true" : "false";
  }
  if (auto const* s = std::get_if<std::string>(&d)) {
    return "\"" + *s + "\"";
  }
  if (auto const* i = std::get_if<std::int64_t>(&d)) {
    return std::to_string(*i);
  }
  if (auto const* fl = std::get_if<double>(&d)) {
    return std::format("{}", *fl);
  }
  return "";
}

auto flag_group_mode_label(flag_group_mode mode) -> std::string_view {
  switch (mode) {
  case flag_group_mode::mutually_exclusive:
    return "mutually exclusive";
  case flag_group_mode::required_one:
    return "at least one required";
  case flag_group_mode::required_exactly_one:
    return "exactly one required";
  }
  return "";
}

auto join_flag_names(std::vector<std::string> const& names) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) {
      out += ", ";
    }
    out += names[i];
  }
  return out;
}

auto render_flag_line(flag const& f) -> std::string {
  std::string label = f.long_name;
  if (f.short_name) {
    label += std::string(", -") + *f.short_name;
  }
  for (auto const& alias : f.aliases) {
    label += ", " + alias;
  }
  std::string out = label + pad_to(label, 22) + "(" + flag_kind_label(f) + ")";
  if (f.list || f.count) {
    out += " (repeatable)";
  }
  if (f.required) {
    out += " required";
  }
  if (f.default_value) {
    out += " default=" + render_default(*f.default_value);
  }
  if (!f.desc.empty()) {
    out += " \xe2\x80\x94 " + f.desc; // em dash, matches etcli's " — " separator
  }
  return out;
}

auto render_flag_group_line(flag_group const& group) -> std::string {
  std::string out = group.name + pad_to(group.name, 20);
  out += std::string(flag_group_mode_label(group.mode)) + ": " + join_flag_names(group.flags);
  if (!group.desc.empty()) {
    out += " \xe2\x80\x94 " + group.desc;
  }
  return out;
}

auto render_env_line(flag const& f) -> std::string {
  if (!f.env) {
    return "";
  }
  std::string out = "  " + *f.env + pad_to(*f.env, 22);
  out += "cli.run fallback for " + f.long_name + "; parse-unaware\n";
  return out;
}

} // namespace

auto render_help(cmd const& root, std::span<std::string const> path) -> std::string {
  const cmd* target = find_cmd(root, path);
  if (target == nullptr) {
    return {};
  }

  auto flags = collect_inherited_flags(root, path);
  flags.insert(flags.end(), target->flags.begin(), target->flags.end());

  std::string out;
  auto const  full_path = render_path(path, target->name);
  out += full_path;
  out += "\n";

  if (!target->long_desc.empty()) {
    out += "\n" + target->long_desc + "\n";
  } else if (!target->desc.empty()) {
    out += "\n  " + target->desc + "\n";
  }

  std::string usage = "\nUSAGE:\n  " + full_path;
  if (has_visible_flags(flags)) {
    usage += " [flags]";
  }
  if (has_visible_commands(target->cmds)) {
    usage += " <command>";
  }
  for (auto const& p : target->positionals) {
    usage += p.required ? (" <" + p.name + ">") : (" [" + p.name + "]");
  }
  out += usage + "\n";

  if (has_visible_commands(target->cmds)) {
    out += "\nCOMMANDS:\n";
    for (auto const& c : target->cmds) {
      if (!visible_cmd(c)) {
        continue;
      }
      auto const label = cmd_label(c);
      out += "  " + label + pad_to(label, 16);
      if (!c.desc.empty()) {
        out += c.desc;
      }
      out += "\n";
    }
  }

  if (has_visible_flags(flags)) {
    out += "\nFLAGS:\n";
    for (auto const& f : flags) {
      if (!visible_flag(f)) {
        continue;
      }
      out += "  " + render_flag_line(f) + "\n";
    }
  }

  if (has_visible_flag_groups(target->flag_groups, flags)) {
    out += "\nFLAG GROUPS:\n";
    for (auto const& group : target->flag_groups) {
      if (!visible_flag_group(group, flags)) {
        continue;
      }
      out += "  " + render_flag_group_line(group) + "\n";
    }
  }

  if (has_visible_env(flags)) {
    out += "\nENVIRONMENT:\n";
    for (auto const& f : flags) {
      if (!visible_flag(f)) {
        continue;
      }
      out += render_env_line(f);
    }
  }

  if (!target->positionals.empty()) {
    out += "\nPOSITIONAL ARGUMENTS:\n";
    for (auto const& p : target->positionals) {
      std::string const name_col = "<" + p.name + ">";
      out += "  " + name_col + pad_to(p.name, 14) + "(" + std::string(kind_label(p.value_kind)) + ")";
      if (!p.required) {
        out += " optional";
      }
      if (p.default_value) {
        out += " default=" + render_default(*p.default_value);
      }
      if (!p.desc.empty()) {
        out += " \xe2\x80\x94 " + p.desc;
      }
      out += "\n";
    }
  }

  return out;
}

} // namespace planar::cli

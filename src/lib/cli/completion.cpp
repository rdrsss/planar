/// @file completion.cpp
/// @brief Implementation of `planar.cli.completion::generate_script`.
module;

module planar.cli.completion;

import std;
import planar.cli.flag;
import planar.cli.cmd;

namespace planar::cli {

namespace {

auto visible_cmd(cmd const& c) -> bool {
  return !c.hidden;
}

auto visible_flag(flag const& f) -> bool {
  return !f.hidden;
}

auto join_path(std::span<std::string const> path) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (i > 0) {
      out += " ";
    }
    out += path[i];
  }
  return out;
}

auto join_cmd_names(std::vector<cmd> const& cmds) -> std::string {
  std::string out;
  bool        first = true;
  for (auto const& c : cmds) {
    if (!visible_cmd(c)) {
      continue;
    }
    if (!first) {
      out += " ";
    }
    first = false;
    out += c.name;
  }
  return out;
}

auto join_flag_names(std::vector<flag> const& flags) -> std::string {
  std::string out;
  bool        first = true;
  for (auto const& f : flags) {
    if (!visible_flag(f)) {
      continue;
    }
    if (!first) {
      out += " ";
    }
    first = false;
    out += f.long_name;
    if (f.short_name) {
      out += std::string(" -") + *f.short_name;
    }
  }
  if (!first) {
    out += " ";
  }
  out += "--help -h";
  return out;
}

auto merged_flag_names(std::vector<flag> const& inherited, std::vector<flag> const& owned) -> std::string {
  std::vector<flag> merged = inherited;
  merged.insert(merged.end(), owned.begin(), owned.end());
  return join_flag_names(merged);
}

auto bash_script(cmd const& root) -> std::string {
  std::string out;
  out += "# " + root.name + " bash completion (auto-generated)\n\n";
  out += "_" + root.name + "() {\n";
  out += "    local cur path cmds flags i\n";
  out += "    cur=\"${COMP_WORDS[COMP_CWORD]}\"\n";
  out += "    path=\"\"\n";
  out += "    for (( i=1; i<COMP_CWORD; i++ )); do\n";
  out += "        case \"${COMP_WORDS[i]}\" in\n";
  out += "            -*) ;;\n";
  out += "            *)\n";
  out += "                if [[ -z \"$path\" ]]; then\n";
  out += "                    path=\"${COMP_WORDS[i]}\"\n";
  out += "                else\n";
  out += "                    path=\"$path ${COMP_WORDS[i]}\"\n";
  out += "                fi\n";
  out += "                ;;\n";
  out += "        esac\n";
  out += "    done\n\n";
  out += "    case \"$path\" in\n";
  out += "        \"\")\n";
  out += "            cmds=\"" + join_cmd_names(root.cmds) + "\"\n";
  out += "            flags=\"" + join_flag_names(root.flags) + "\"\n";
  out += "            ;;\n";

  for (auto const& n : all_nodes(root)) {
    if (!visible_cmd(*n.node)) {
      continue;
    }
    out += "        \"" + join_path(n.path) + "\")\n";
    out += "            cmds=\"" + join_cmd_names(n.node->cmds) + "\"\n";
    auto inherited = collect_inherited_flags(root, n.path);
    out += "            flags=\"" + merged_flag_names(inherited, n.node->flags) + "\"\n";
    out += "            ;;\n";
  }

  out += "        *)\n";
  out += "            cmds=\"\"\n";
  out += "            flags=\"\"\n";
  out += "            ;;\n";
  out += "    esac\n\n";
  out += "    if [[ \"$cur\" == -* ]]; then\n";
  out += "        COMPREPLY=( $(compgen -W \"$flags\" -- \"$cur\") )\n";
  out += "    else\n";
  out += "        COMPREPLY=( $(compgen -W \"$cmds\" -- \"$cur\") )\n";
  out += "    fi\n";
  out += "}\n\n";
  out += "complete -F _" + root.name + " " + root.name + "\n";
  return out;
}

auto zsh_escape_desc(std::string_view s) -> std::string {
  std::string out;
  for (char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case ':':
      out += "\\:";
      break;
    case '$':
      out += "\\$";
      break;
    case '`':
      out += "\\`";
      break;
    default:
      out += c;
    }
  }
  return out;
}

auto zsh_cmd_pairs(std::vector<cmd> const& cmds) -> std::string {
  std::string out;
  bool        first = true;
  for (auto const& c : cmds) {
    if (!visible_cmd(c)) {
      continue;
    }
    if (!first) {
      out += " ";
    }
    first = false;
    out += "\"" + c.name + ":" + zsh_escape_desc(c.desc) + "\"";
  }
  return out;
}

auto zsh_flag_pairs(std::vector<flag> const& flags) -> std::string {
  std::string out;
  bool        first = true;
  for (auto const& f : flags) {
    if (!visible_flag(f)) {
      continue;
    }
    if (!first) {
      out += " ";
    }
    first = false;
    out += "\"" + f.long_name + ":" + zsh_escape_desc(f.desc) + "\"";
  }
  if (!first) {
    out += " ";
  }
  out += "\"--help:Show help\" \"-h:Show help\"";
  return out;
}

auto zsh_script(cmd const& root) -> std::string {
  std::string out;
  out += "#compdef " + root.name + "\n";
  out += "# " + root.name + " zsh completion (auto-generated)\n\n";
  out += "_" + root.name + "() {\n";
  out += "    local cur path i\n";
  out += "    cur=\"${words[CURRENT]}\"\n";
  out += "    path=\"\"\n";
  out += "    for (( i=2; i<CURRENT; i++ )); do\n";
  out += "        case \"${words[i]}\" in\n";
  out += "            -*) ;;\n";
  out += "            *)\n";
  out += "                if [[ -z \"$path\" ]]; then\n";
  out += "                    path=\"${words[i]}\"\n";
  out += "                else\n";
  out += "                    path=\"$path ${words[i]}\"\n";
  out += "                fi\n";
  out += "                ;;\n";
  out += "        esac\n";
  out += "    done\n\n";
  out += "    local -a cmds flags\n";
  out += "    case \"$path\" in\n";
  out += "        \"\")\n";
  out += "            cmds=(" + zsh_cmd_pairs(root.cmds) + ")\n";
  out += "            flags=(" + zsh_flag_pairs(root.flags) + ")\n";
  out += "            ;;\n";

  for (auto const& n : all_nodes(root)) {
    if (!visible_cmd(*n.node)) {
      continue;
    }
    out += "        \"" + join_path(n.path) + "\")\n";
    out += "            cmds=(" + zsh_cmd_pairs(n.node->cmds) + ")\n";
    auto inherited = collect_inherited_flags(root, n.path);
    inherited.insert(inherited.end(), n.node->flags.begin(), n.node->flags.end());
    out += "            flags=(" + zsh_flag_pairs(inherited) + ")\n";
    out += "            ;;\n";
  }

  out += "    esac\n\n";
  out += "    if [[ \"$cur\" == -* ]]; then\n";
  out += "        _describe -t flags 'flags' flags\n";
  out += "    else\n";
  out += "        _describe -t commands 'commands' cmds\n";
  out += "    fi\n";
  out += "}\n\n";
  out += "_" + root.name + " \"$@\"\n";
  return out;
}

auto fish_single_quote(std::string_view s) -> std::string {
  std::string out;
  for (char c : s) {
    if (c == '\\') {
      out += "\\\\";
    } else if (c == '\'') {
      out += "\\'";
    } else {
      out += c;
    }
  }
  return out;
}

auto fish_cmd_line(std::string_view bin, std::string_view path, cmd const& c) -> std::string {
  std::string out = "complete -c " + std::string(bin) + " -n '__fish_" + std::string(bin) + "_path \"" + std::string(path) +
                    "\"' -f -a '" + fish_single_quote(c.name) + "'";
  if (!c.desc.empty()) {
    out += " -d '" + fish_single_quote(c.desc) + "'";
  }
  out += "\n";
  return out;
}

auto fish_flag_line(std::string_view bin, std::string_view path, flag const& f) -> std::string {
  std::string_view long_bare = f.long_name;
  if (long_bare.starts_with("--")) {
    long_bare = long_bare.substr(2);
  } else if (long_bare.starts_with("-")) {
    long_bare = long_bare.substr(1);
  }
  std::string out = "complete -c " + std::string(bin) + " -n '__fish_" + std::string(bin) + "_path \"" + std::string(path) +
                    "\"' -f -l '" + fish_single_quote(long_bare) + "'";
  if (f.short_name) {
    out += std::string(" -s ") + *f.short_name;
  }
  if (!f.desc.empty()) {
    out += " -d '" + fish_single_quote(f.desc) + "'";
  }
  out += "\n";
  return out;
}

auto fish_script(cmd const& root) -> std::string {
  std::string out;
  out += "# " + root.name + " fish completion (auto-generated)\n\n";
  out += "function __fish_" + root.name + "_path\n";
  out += "    set -l cmd (commandline -opc)\n";
  out += "    set -l path\n";
  out += "    set -l first 1\n";
  out += "    for word in $cmd[2..]\n";
  out += "        switch $word\n";
  out += "            case '-*'\n";
  out += "                continue\n";
  out += "        end\n";
  out += "        if test $first -eq 1\n";
  out += "            set path $word\n";
  out += "            set first 0\n";
  out += "        else\n";
  out += "            set path \"$path $word\"\n";
  out += "        end\n";
  out += "    end\n";
  out += "    test \"$path\" = \"$argv[1]\"\n";
  out += "end\n\n";

  for (auto const& c : root.cmds) {
    if (!visible_cmd(c)) {
      continue;
    }
    out += fish_cmd_line(root.name, "", c);
  }
  for (auto const& f : root.flags) {
    if (!visible_flag(f)) {
      continue;
    }
    out += fish_flag_line(root.name, "", f);
  }

  for (auto const& n : all_nodes(root)) {
    if (!visible_cmd(*n.node)) {
      continue;
    }
    auto const path_str = join_path(n.path);
    for (auto const& c : n.node->cmds) {
      if (!visible_cmd(c)) {
        continue;
      }
      out += fish_cmd_line(root.name, path_str, c);
    }
    for (auto const& f : n.node->flags) {
      if (!visible_flag(f)) {
        continue;
      }
      out += fish_flag_line(root.name, path_str, f);
    }
  }

  return out;
}

} // namespace

auto generate_script(cmd const& root, shell sh) -> std::string {
  switch (sh) {
  case shell::bash:
    return bash_script(root);
  case shell::zsh:
    return zsh_script(root);
  case shell::fish:
    return fish_script(root);
  }
  return {};
}

} // namespace planar::cli

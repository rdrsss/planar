/// @file completion.cpp
/// @brief Implementation of `planar.cliapp.completion::generate_script`.

module planar.cliapp.completion;

import std;
import cli11;
import planar.cliapp.walk;

namespace planar::cliapp {

namespace {

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

auto join_cmd_names(const CLI::App& node) -> std::string {
  std::string out;
  bool        first = true;
  for (const CLI::App* child : children(node)) {
    if (!first) {
      out += " ";
    }
    first = false;
    out += child->get_name();
  }
  return out;
}

auto join_flag_names(std::span<const CLI::Option* const> flags) -> std::string {
  std::string out;
  bool        first = true;
  for (const CLI::Option* opt : flags) {
    if (!first) {
      out += " ";
    }
    first = false;
    out += canonical_name(*opt);
    auto const& shorts = opt->get_snames();
    if (!shorts.empty()) {
      out += " -" + shorts.front();
    }
  }
  if (!first) {
    out += " ";
  }
  out += "--help -h";
  return out;
}

auto visible_flags_at(const CLI::App& root, std::span<std::string const> path, const CLI::App& node)
    -> std::vector<const CLI::Option*> {
  auto merged = inherited_flags(root, path);
  auto owned  = local_flags(node);
  merged.insert(merged.end(), owned.begin(), owned.end());
  return merged;
}

auto bash_script(const CLI::App& root) -> std::string {
  auto const  name = root.get_name();
  std::string out;
  out += "# " + name + " bash completion (auto-generated)\n\n";
  out += "_" + name + "() {\n";
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
  out += "            cmds=\"" + join_cmd_names(root) + "\"\n";
  out += "            flags=\"" + join_flag_names(local_flags(root)) + "\"\n";
  out += "            ;;\n";

  for (auto const& node : all_nodes(root)) {
    out += "        \"" + join_path(node.path) + "\")\n";
    out += "            cmds=\"" + join_cmd_names(*node.node) + "\"\n";
    out += "            flags=\"" + join_flag_names(visible_flags_at(root, node.path, *node.node)) + "\"\n";
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
  out += "complete -F _" + name + " " + name + "\n";
  return out;
}

auto zsh_escape_desc(std::string_view text) -> std::string {
  std::string out;
  for (char const c : text) {
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

auto zsh_cmd_pairs(const CLI::App& node) -> std::string {
  std::string out;
  bool        first = true;
  for (const CLI::App* child : children(node)) {
    if (!first) {
      out += " ";
    }
    first = false;
    out += "\"" + child->get_name() + ":" + zsh_escape_desc(child->get_description()) + "\"";
  }
  return out;
}

auto zsh_flag_pairs(std::span<const CLI::Option* const> flags) -> std::string {
  std::string out;
  bool        first = true;
  for (const CLI::Option* opt : flags) {
    if (!first) {
      out += " ";
    }
    first = false;
    out += "\"" + canonical_name(*opt) + ":" + zsh_escape_desc(opt->get_description()) + "\"";
  }
  if (!first) {
    out += " ";
  }
  out += "\"--help:Show help\" \"-h:Show help\"";
  return out;
}

auto zsh_script(const CLI::App& root) -> std::string {
  auto const  name = root.get_name();
  std::string out;
  out += "#compdef " + name + "\n";
  out += "# " + name + " zsh completion (auto-generated)\n\n";
  out += "_" + name + "() {\n";
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
  out += "            cmds=(" + zsh_cmd_pairs(root) + ")\n";
  out += "            flags=(" + zsh_flag_pairs(local_flags(root)) + ")\n";
  out += "            ;;\n";

  for (auto const& node : all_nodes(root)) {
    out += "        \"" + join_path(node.path) + "\")\n";
    out += "            cmds=(" + zsh_cmd_pairs(*node.node) + ")\n";
    out += "            flags=(" + zsh_flag_pairs(visible_flags_at(root, node.path, *node.node)) + ")\n";
    out += "            ;;\n";
  }

  out += "    esac\n\n";
  out += "    if [[ \"$cur\" == -* ]]; then\n";
  out += "        _describe -t flags 'flags' flags\n";
  out += "    else\n";
  out += "        _describe -t commands 'commands' cmds\n";
  out += "    fi\n";
  out += "}\n\n";
  out += "_" + name + " \"$@\"\n";
  return out;
}

auto fish_single_quote(std::string_view text) -> std::string {
  std::string out;
  for (char const c : text) {
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

auto fish_cmd_line(std::string_view bin, std::string_view path, const CLI::App& child) -> std::string {
  std::string out = "complete -c " + std::string(bin) + " -n '__fish_" + std::string(bin) + "_path \"" + std::string(path) +
                    "\"' -f -a '" + fish_single_quote(child.get_name()) + "'";
  auto const  description = child.get_description();
  if (!description.empty()) {
    out += " -d '" + fish_single_quote(description) + "'";
  }
  out += "\n";
  return out;
}

auto fish_flag_line(std::string_view bin, std::string_view path, const CLI::Option& opt) -> std::string {
  std::string const canonical = canonical_name(opt);
  std::string_view  long_bare = canonical;
  if (long_bare.starts_with("--")) {
    long_bare = long_bare.substr(2);
  } else if (long_bare.starts_with("-")) {
    long_bare = long_bare.substr(1);
  }
  std::string out    = "complete -c " + std::string(bin) + " -n '__fish_" + std::string(bin) + "_path \"" + std::string(path) +
                       "\"' -f -l '" + fish_single_quote(long_bare) + "'";
  auto const& shorts = opt.get_snames();
  if (!shorts.empty()) {
    out += " -s " + shorts.front();
  }
  if (!opt.get_description().empty()) {
    out += " -d '" + fish_single_quote(opt.get_description()) + "'";
  }
  out += "\n";
  return out;
}

auto fish_script(const CLI::App& root) -> std::string {
  auto const  name = root.get_name();
  std::string out;
  out += "# " + name + " fish completion (auto-generated)\n\n";
  out += "function __fish_" + name + "_path\n";
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

  for (const CLI::App* child : children(root)) {
    out += fish_cmd_line(name, "", *child);
  }
  for (const CLI::Option* opt : local_flags(root)) {
    out += fish_flag_line(name, "", *opt);
  }

  for (auto const& node : all_nodes(root)) {
    auto const path_str = join_path(node.path);
    for (const CLI::App* child : children(*node.node)) {
      out += fish_cmd_line(name, path_str, *child);
    }
    for (const CLI::Option* opt : local_flags(*node.node)) {
      out += fish_flag_line(name, path_str, *opt);
    }
  }

  return out;
}

} // namespace

auto generate_script(const CLI::App& root, shell sh) -> std::string {
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

} // namespace planar::cliapp

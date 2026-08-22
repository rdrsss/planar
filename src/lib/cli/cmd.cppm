/// @file cmd.cppm
/// @brief `planar.cli.cmd` — the `cmd` tree-node type and the tree-walk
/// helpers the parser, help renderer, and completion generator all share.
///
/// Behavior-preserving port of zig/vendor/etcli/src/cli/cmd.zig (D2, D9).
/// The Zig source is `Cmd` as pure comptime data plus a family of
/// `comptime`-only tree-traversal functions (`findCmd`, `allNodes`,
/// `allLeaves`, `collectInheritedFlags`) that exist ONLY to feed the
/// per-leaf `ArgsType` struct generator (`cmd_mod.ArgsType`). This port has
/// no `ArgsType` generator (see flag.cppm's file comment) and no `run`
/// handler-dispatch field — those are engine/cmd concerns explicitly out of
/// scope for this task. What survives, unchanged in shape, is the tree
/// itself and ordinary runtime tree-walks: `cmd` is a plain aggregate (no
/// constexpr requirement — nothing here needs compile-time evaluation to
/// pay for itself), and `find_cmd`/`all_nodes`/`all_leaves`/
/// `collect_inherited_flags` are ordinary functions a later schema-catalog
/// pass (task 6029) can call directly instead of re-deriving via
/// `@setEvalBranchQuota` comptime recursion.
module;

export module planar.cli.cmd;

import std;
import planar.cli.flag;

namespace planar::cli {

/// @brief One node in the command tree. `cmds` nests sub-commands;
/// `long_desc` (when non-empty) is the multi-line prose lead-in for this
/// command's own help page, while `desc` stays the one-line summary shown
/// in the parent's COMMANDS table (etcli's `long_desc`/`desc` split).
export struct cmd {
  std::string              name;           ///< The command's own name (matched literally against argv).
  std::vector<std::string> aliases;        ///< Additional names that also resolve to this command.
  bool                     hidden = false; ///< Excluded from help/completion listings when true.
  std::string              desc;           ///< One-line summary shown in the parent's COMMANDS table.
  std::string              long_desc;      ///< Optional multi-line prose lead-in for this command's own help page.
  std::vector<flag>        flags;          ///< Flags declared directly on this command (not inherited).
  std::vector<positional>  positionals;    ///< Positional arguments, consumed in declaration order.
  /// Command-level flag relationships; members reference canonical long
  /// flag names visible at this command path, including inherited flags.
  std::vector<flag_group> flag_groups;
  /// When true, the parser ignores unknown `-x`/`--long` tokens at this
  /// leaf (and best-effort-swallows one following non-dash token as its
  /// value). Reserved for forwarding/legacy passthrough leaves — prefer
  /// declaring flags explicitly.
  bool allow_unknown_flags = false;
  /// When true, the parser ignores extra positional tokens beyond the
  /// declared `positionals` for this leaf.
  bool allow_extra_positionals = false;
  /// When set, extra positional tokens beyond `positionals` are collected
  /// under this synthesized result key (`match_result::rest`), implying
  /// `allow_extra_positionals`.
  std::optional<std::string> rest_field;
  std::vector<cmd>           cmds; ///< Nested sub-commands; a childless node is a leaf (see `all_leaves`).
};

/// @brief A `(path, node)` pair returned by `all_nodes`/`all_leaves`.
export struct node_ref {
  std::vector<std::string> path;           ///< The node's root-relative path.
  const cmd*               node = nullptr; ///< The node itself (borrowed; the tree outlives this reference).
};

/// @brief True if `tok` names `c` by its primary name or a declared alias.
/// @param c The candidate command node.
/// @param tok The argv token to test.
/// @return `true` if `tok` matches `c.name` or one of `c.aliases`.
export auto command_matches(cmd const& c, std::string_view tok) -> bool {
  if (c.name == tok) {
    return true;
  }
  return std::ranges::any_of(c.aliases, [&](std::string const& alias) { return alias == tok; });
}

/// @brief Resolve a command in the tree by its name path.
/// @param root The tree root to search from.
/// @param path A sequence of subcommand names/aliases, root-to-leaf.
/// @return The resolved node, or `nullptr` if `path` does not resolve.
export auto find_cmd(cmd const& root, std::span<std::string const> path) -> const cmd* {
  const cmd* current = &root;
  for (auto const& seg : path) {
    const cmd* matched = nullptr;
    for (auto const& child : current->cmds) {
      if (command_matches(child, seg)) {
        matched = &child;
        break;
      }
    }
    if (matched == nullptr) {
      return nullptr;
    }
    current = matched;
  }
  return current;
}

namespace detail {

/// @brief Recursive worker for `all_nodes`: depth-first pre-order walk
/// appending every non-root `(prefix, node)` pair to `out`.
/// @param node The subtree root being visited.
/// @param prefix `node`'s path from the tree root.
/// @param out Accumulator, appended to in place.
inline auto walk_nodes(cmd const& node, std::vector<std::string> const& prefix, std::vector<node_ref>& out) -> void {
  if (!prefix.empty()) {
    out.push_back({prefix, &node});
  }
  for (auto const& child : node.cmds) {
    auto next_prefix = prefix;
    next_prefix.push_back(child.name);
    walk_nodes(child, next_prefix, out);
  }
}

/// @brief Recursive worker for `all_leaves`: depth-first pre-order walk
/// appending every non-root, childless `(prefix, node)` pair to `out`.
/// @param node The subtree root being visited.
/// @param prefix `node`'s path from the tree root.
/// @param out Accumulator, appended to in place.
inline auto walk_leaves(cmd const& node, std::vector<std::string> const& prefix, std::vector<node_ref>& out) -> void {
  // A node is a leaf when it has no children — this port carries no `run`
  // handler field (see file comment), so etcli's `node.run != null or
  // node.cmds.len == 0` collapses to just the second disjunct here.
  bool const is_leaf = node.cmds.empty();
  if (is_leaf && !prefix.empty()) {
    out.push_back({prefix, &node});
  }
  for (auto const& child : node.cmds) {
    auto next_prefix = prefix;
    next_prefix.push_back(child.name);
    walk_leaves(child, next_prefix, out);
  }
}

} // namespace detail

/// @brief Gather every node in the tree (leaves AND parents), excluding the
/// root itself. Used by help/completion so `tool group --help` (or a
/// completion request under an intermediate path) resolves against that
/// group's own node, not just the leaves beneath it.
/// @param root The tree root to walk.
/// @return Every non-root node paired with its root-relative path.
export auto all_nodes(cmd const& root) -> std::vector<node_ref> {
  std::vector<node_ref> out;
  detail::walk_nodes(root, {}, out);
  return out;
}

/// @brief Gather every leaf command (any node with no children). Excludes
/// the root when the root itself has no children (matches etcli: a leaf
/// entry requires `prefix.len > 0`, so a childless root yields no leaves).
/// @param root The tree root to walk.
/// @return Every leaf node paired with its root-relative path.
export auto all_leaves(cmd const& root) -> std::vector<node_ref> {
  std::vector<node_ref> out;
  detail::walk_leaves(root, {}, out);
  return out;
}

/// @brief Collect flag specs from every ancestor of the command at `path`,
/// root-first, immediate-parent-last. Excludes the target's own flags.
/// @param root The tree root.
/// @param path The path to the target command (may be empty, yielding no
/// inherited flags).
/// @return The concatenated ancestor flag specs, root to immediate parent.
export auto collect_inherited_flags(cmd const& root, std::span<std::string const> path) -> std::vector<flag> {
  std::vector<flag> out;
  if (path.empty()) {
    return out;
  }
  out.insert(out.end(), root.flags.begin(), root.flags.end());
  const cmd* current = &root;
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    const cmd* matched = nullptr;
    for (auto const& child : current->cmds) {
      if (command_matches(child, path[i])) {
        matched = &child;
        break;
      }
    }
    if (matched == nullptr) {
      break;
    }
    out.insert(out.end(), matched->flags.begin(), matched->flags.end());
    current = matched;
  }
  return out;
}

} // namespace planar::cli

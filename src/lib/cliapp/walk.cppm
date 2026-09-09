/// @file walk.cppm
/// @brief `planar.cliapp.walk` — the tree-walk primitives over a built
/// `CLI::App` that the schema-catalog emitter and the completion generator
/// both need (plan 996, task 6123).
///
/// ## Why this module exists at all
///
/// Task 6123 deleted `src/lib/cli` and made each `src/cmd/<binary>/`
/// declare its command tree directly as a `CLI::App`. That removes the
/// hand-rolled parser, the `cmd`/`flag` tree types, and the help renderer
/// — CLI11 owns all four now. It does NOT remove two things CLI11 has no
/// equivalent for and which are OPERATOR/TOOLING contracts rather than
/// parser concerns:
///
///   - the `<bin> schema` JSON catalog `zig/tools/cli_usage_lint` consumes
///     (`planar.cliapp.schema`), and
///   - the shell-completion script generator (`planar.cliapp.completion`).
///
/// Both are pure FUNCTIONS OF a `CLI::App`. They take a tree someone else
/// built and describe it; they hold no tree of their own, declare no verb,
/// and cannot influence how argv is parsed. That is why they are layer-1
/// and shared rather than duplicated per binary: sharing them is not
/// sharing a command tree. D18's prohibition is on a `cmd_* -> cmd_*` edge
/// — the four binaries must not reach sideways into each other's trees —
/// and a layer-1 library both may depend on downward is exactly the shape
/// D19 already established for `scope_ref` and `json_text`.
///
/// The alternative (a second and third verbatim copy of a 400-line JSON
/// emitter, one per binary that answers `schema`) was rejected on the same
/// grounds `json_text` was extracted: ten copies of one escape table had
/// already silently diverged in three of them.
///
/// ## Visibility
///
/// CLI11 hides a subcommand or an option by putting it in the empty group
/// (`->group("")`). Both walks below treat an empty group as hidden and
/// PRUNE the subtree beneath a hidden subcommand rather than merely
/// skipping the node itself — a visible child of a hidden parent is not
/// reachable from argv, so listing it in a catalog or a completion script
/// would be a lie about the surface.
module;

export module planar.cliapp.walk;

import std;
import cli11;

namespace planar::cliapp {

/// @brief A `(path, node)` pair produced by `all_nodes`.
export struct node_ref {
  std::vector<std::string> path;           ///< The node's root-relative subcommand path.
  const CLI::App*          node = nullptr; ///< The node itself (borrowed; the tree outlives this reference).
};

/// @brief Whether `app` is visible in help/completion/catalog listings.
/// CLI11 hides a subcommand by assigning it the empty group.
/// @param app The subcommand node.
/// @return `true` when the node should be listed.
export auto visible(const CLI::App& app) -> bool {
  return !app.get_group().empty();
}

/// @brief Whether `opt` is visible in help/completion/catalog listings.
/// @param opt The option.
/// @return `true` when the option should be listed.
export auto visible(const CLI::Option& opt) -> bool {
  return !opt.get_group().empty();
}

/// @brief The options declared directly on `app`, excluding CLI11's
/// auto-added `--help` flag and excluding positionals.
///
/// `--help` is excluded because it is not part of the declared surface:
/// `zig/tools/cli_usage_lint` carries it in its own `global_ok_flags`
/// list precisely because no binary's catalog has ever listed it, and
/// emitting it now would change every command entry in the catalog.
/// @param app The node whose own options to collect.
/// @return The visible, non-positional, non-help options in declaration order.
export auto local_flags(const CLI::App& app) -> std::vector<const CLI::Option*> {
  std::vector<const CLI::Option*> out;
  const CLI::Option*              help = app.get_help_ptr();
  for (const CLI::Option* opt : app.get_options()) {
    if (opt == help || opt->get_positional() || !visible(*opt)) {
      continue;
    }
    out.push_back(opt);
  }
  return out;
}

/// @brief The positionals declared directly on `app`, in declaration order.
/// @param app The node whose own positionals to collect.
/// @return The visible positional options.
export auto local_positionals(const CLI::App& app) -> std::vector<const CLI::Option*> {
  std::vector<const CLI::Option*> out;
  for (const CLI::Option* opt : app.get_options()) {
    if (!opt->get_positional() || !visible(*opt)) {
      continue;
    }
    out.push_back(opt);
  }
  return out;
}

/// @brief The visible direct subcommands of `app`, in declaration order.
/// @param app The parent node.
/// @return The visible children.
export auto children(const CLI::App& app) -> std::vector<const CLI::App*> {
  std::vector<const CLI::App*> out;
  for (const CLI::App* child : app.get_subcommands([](const CLI::App*) { return true; })) {
    if (visible(*child)) {
      out.push_back(child);
    }
  }
  return out;
}

namespace detail {

/// @brief Recursive worker for `all_nodes`: depth-first pre-order,
/// pruning at a hidden node.
/// @param node The subtree root being visited.
/// @param prefix `node`'s path from the tree root.
/// @param out Accumulator, appended to in place.
inline auto walk(const CLI::App& node, std::vector<std::string> const& prefix, std::vector<node_ref>& out) -> void {
  if (!prefix.empty()) {
    out.push_back({prefix, &node});
  }
  for (const CLI::App* child : children(node)) {
    auto next = prefix;
    next.push_back(child->get_name());
    walk(*child, next, out);
  }
}

} // namespace detail

/// @brief Every visible non-root node in the tree, paired with its
/// root-relative path, depth-first pre-order.
/// @param root The tree root to walk.
/// @return The nodes.
export auto all_nodes(const CLI::App& root) -> std::vector<node_ref> {
  std::vector<node_ref> out;
  detail::walk(root, {}, out);
  return out;
}

/// @brief Resolve a node by its root-relative subcommand path.
/// @param root The tree root.
/// @param path The subcommand names, root-to-leaf.
/// @return The node, or `nullptr` when `path` does not resolve.
export auto find_node(const CLI::App& root, std::span<std::string const> path) -> const CLI::App* {
  const CLI::App* current = &root;
  for (auto const& segment : path) {
    const CLI::App* matched = nullptr;
    for (const CLI::App* child : children(*current)) {
      if (child->get_name() == segment) {
        matched = child;
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

/// @brief The flags declared on every ANCESTOR of the node at `path`,
/// root-first, immediate-parent-last. Excludes the target's own flags.
/// @param root The tree root.
/// @param path The path to the target node.
/// @return The ancestors' visible flags.
export auto inherited_flags(const CLI::App& root, std::span<std::string const> path) -> std::vector<const CLI::Option*> {
  std::vector<const CLI::Option*> out;
  if (path.empty()) {
    return out;
  }
  const CLI::App* current = &root;
  auto            owned   = local_flags(*current);
  out.insert(out.end(), owned.begin(), owned.end());
  for (std::size_t i = 0; i + 1 < path.size(); ++i) {
    const CLI::App* matched = nullptr;
    for (const CLI::App* child : children(*current)) {
      if (child->get_name() == path[i]) {
        matched = child;
        break;
      }
    }
    if (matched == nullptr) {
      break;
    }
    current    = matched;
    auto stage = local_flags(*current);
    out.insert(out.end(), stage.begin(), stage.end());
  }
  return out;
}

/// @brief The canonical long name of `opt`, e.g. `"--json"`, or its
/// positional name when it is a positional.
///
/// The positional branch reads `get_single_name()`, NOT `get_name(true,
/// false)` (plan 996, task 6358's fix). Both agree for a VISIBLE
/// positional -- CLI11's `get_name` falls through to the identical
/// `return pname_;` arm once `all_options` is false -- but `get_name`
/// carries an UNCONDITIONAL early return, `if (get_group().empty()) return
/// {};`, that fires before that arm is ever reached. A `->group("")`-hidden
/// positional (the mechanism `handlers/capture.cpp`'s `declare_capture`
/// uses to keep
/// `capture commits`'s trailing SHA list out of the `schema` catalog, since
/// the oracle's own `rest_field` never appears there either) therefore
/// canonicalized to the EMPTY STRING, and `harvest()` in `args.cppm` keyed
/// every parsed SHA into `positional_lists[""]` instead of
/// `positional_lists["shas"]` -- silently dropping every commit SHA the
/// operator passed. `positional_string(args, "shas")` came back empty, the
/// handler's `since.has_value() == shas.empty()` both-absent check fired,
/// and `capture commits <sha>` refused with "provide --since <ref> or one
/// or more commit SHAs" no matter what was typed. `get_single_name()` has
/// no such group check -- it reads `pname_` directly for any positional,
/// visible or not -- so it resolves correctly in both cases. See
/// walk.t.cpp's hidden-positional case for the regression this closes.
/// @param opt The option.
/// @return The canonical name.
export auto canonical_name(const CLI::Option& opt) -> std::string {
  if (opt.get_positional()) {
    return opt.get_single_name();
  }
  auto const& longs = opt.get_lnames();
  if (!longs.empty()) {
    return "--" + longs.front();
  }
  auto const& shorts = opt.get_snames();
  if (!shorts.empty()) {
    return "-" + shorts.front();
  }
  return opt.get_name(false, false);
}

} // namespace planar::cliapp

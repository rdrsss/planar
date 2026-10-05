/// @file command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.tree`.

module planar.cmd.planar.handlers.tree;

import std;
import planar.cliapp.args;
import planar.engine.tree;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;
import cli11;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace tree_engine = engine::tree;

namespace {

/// @brief Map a walk failure onto the oracle's refusal text and exit code.
///
/// `slug_not_found` is exit 1 while `unknown_kind` is exit 2 — the two
/// refusals this verb can raise do NOT share a bucket, and both were
/// captured.
auto map_tree_error(tree_engine::tree_error err) -> domain_error {
  switch (err) {
  case tree_engine::tree_error::unsupported_scope:
    return error_from_body(domain_error_kind::invalid_input, "unsupported scope form");
  case tree_engine::tree_error::slug_not_found:
    return error_from_body(domain_error_kind::not_found, "scope slug not found");
  case tree_engine::tree_error::unknown_kind:
    return error_from_body(domain_error_kind::invalid_input, "unknown kind");
  case tree_engine::tree_error::query_failed:
    break;
  }
  return error_from_body(domain_error_kind::generic_failure, "tree walk failed: QueryFailed");
}

} // namespace

auto tree(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  tree_engine::tree_filter filter;
  filter.all_scopes = cliapp::flag_bool(args, "--all-scopes");

  // `--depth` defaults to -1, and ANY value <= 0 is unbounded: `--depth 0`
  // renders identically to `--depth -1` (captured, not assumed).
  filter.max_depth = cliapp::flag_int(args, "--depth").value_or(-1);

  // `--kind` is a SINGLE value, not a list, and it is validated HERE so the
  // refusal quotes the operator's own text. The empty string is a reachable
  // case and refuses as `unknown kind ''` — it does NOT mean "all kinds".
  if (auto const kind = cliapp::flag_string(args, "--kind"); kind.has_value()) {
    if (!tree_engine::is_valid_kind(*kind)) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, tree_engine::render_unknown_kind(*kind)));
    }
    filter.kinds.push_back(*kind);
  }

  // `--status` is also single-valued, and unlike `--kind` it is NOT
  // validated against any set: an unknown status matches nothing and exits
  // 0. An empty `--status` likewise matches nothing rather than everything.
  if (auto const status = cliapp::flag_string(args, "--status"); status.has_value()) {
    filter.statuses.push_back(*status);
  }

  // TASK 6281: `--sort` is READ. It used to be read and discarded -- the
  // oracle declared the flag, stored it on its filter and never consulted
  // it, so `--sort updated` and `--sort bogus` were both byte-identical to a
  // bare `tree` and an unrecognised key did not refuse. Reproduced under D2
  // until decision 1067 ended that rule.
  //
  // A BOGUS KEY NOW REFUSES. Silently accepting one is the worse half of the
  // defect: an operator who typos `--sort updatd` got a tree ordered some
  // other way and no indication of it.
  if (auto const raw = cliapp::flag_string(args, "--sort"); raw.has_value()) {
    auto const parsed = tree_engine::sort_key_from_text(*raw);
    if (!parsed) {
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input,
                          std::format("unknown --sort value '{}'; expected one of: id, updated, created, unsorted", *raw)));
    }
    filter.sort = *parsed;
  }

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  std::vector<tree_engine::node> roots;

  auto const scope_flag = cliapp::flag_string(args, "--scope");
  if (filter.all_scopes) {
    auto built = tree_engine::build(**conn, filter);
    if (!built) {
      return std::unexpected(map_tree_error(built.error()));
    }
    roots = std::move(*built);
  } else if (scope_flag.has_value()) {
    // An EMPTY `--scope` is global, not cross-scope and not a refusal.
    // Passed straight to the engine so an unknown slug reports the
    // engine's own message, exactly as the oracle routes it.
    if (!scope_flag->empty()) {
      filter.scope = *scope_flag;
    }
    auto built = tree_engine::build(**conn, filter);
    if (!built) {
      return std::unexpected(map_tree_error(built.error()));
    }
    roots = std::move(*built);
  } else {
    // No `--scope`: derive the read set from the cwd. An empty read set is
    // the shared refusal (exit 1), which this verb's oracle wording matches
    // byte-for-byte.
    auto slugs = resolve_read_scope_slugs(ctx);
    if (!slugs) {
      return std::unexpected(slugs.error());
    }
    for (auto const& slug : *slugs) {
      auto scoped  = filter;
      scoped.scope = slug;
      auto built   = tree_engine::build(**conn, scoped);
      if (!built) {
        return std::unexpected(map_tree_error(built.error()));
      }
      std::ranges::move(*built, std::back_inserter(roots));
    }
  }

  ctx.out() << (cliapp::flag_bool(args, "--json") ? tree_engine::render_json(roots) : tree_engine::render_text(roots));
  return {};
}

/// @brief Declare the `tree` leaf.
///
/// The `tree` verb, attached by the root assembly in
/// `planar.cmd.planar.main`.
auto declare_tree(CLI::App& root) -> void {
  CLI::App* tree =
      root.add_subcommand("tree", "Render a hierarchical view of Planar entities for one or all\n  scopes.\n\n  Walks plans (via "
                                  "parent_plan_id), tasks (via plan_id and\n  parent_task_id), and entity_links(derives-from) to "
                                  "gather\n  artifacts, decisions, scenarios, and questions linked to each plan.");
  add_string(*tree, "--scope", "Limit to a single scope slug");
  add_bool(*tree, "--all-scopes", "Include every scope");
  add_int_default(*tree, "--depth", "-1", "Max tree depth (-1 = unbounded)");
  add_string(*tree, "--kind", "Restrict to a single kind");
  add_string(*tree, "--status", "Restrict to a single status");
  add_string(*tree, "--sort", "Sort key: id, updated, created, unsorted");
  add_json(*tree, "Emit machine-readable JSON instead of text");
}

} // namespace planar::cmd::handlers

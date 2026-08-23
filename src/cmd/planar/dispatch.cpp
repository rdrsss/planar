/// @file dispatch.cpp
/// @brief Implementation of `planar.cmd.planar.dispatch`.

module planar.cmd.planar.dispatch;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.walk;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.annotate;
import planar.cmd.planar.handlers.skills;
import planar.cmd.planar.handlers.unlink;
import planar.cmd.planar.handlers.version;
import planar.cmd.planar.handlers.workbench;
import planar.cmd.planar.handlers.workflow;
import planar.cmd.planar.handlers.workspace;

namespace planar::cmd {

namespace {

/// @brief The deepest node CLI11 actually matched, and the path taken to
/// reach it.
///
/// `CLI::App::get_subcommands()` returns the PARSED children at each
/// level, so following `.front()` down walks exactly the chain argv
/// selected. A node reached this way that still has children of its own
/// means the operator named a group without naming a leaf under it —
/// `run` renders that node's help page, matching what the deleted parser
/// did for a bare parent verb.
/// @param root The parsed root app.
/// @return The matched node and its root-relative path.
auto matched_node(CLI::App& root) -> std::pair<CLI::App*, std::vector<std::string>> {
  CLI::App*                node = &root;
  std::vector<std::string> path;
  while (true) {
    auto const matched = node->get_subcommands();
    if (matched.empty()) {
      return {node, path};
    }
    node = matched.front();
    path.push_back(node->get_name());
  }
}

} // namespace

auto handlers() -> handler_table {
  handler_table table;
  table.emplace("version", handlers::version);
  table.emplace("workflow list", handlers::workflow_list);
  table.emplace("workflow show", handlers::workflow_show);
  table.emplace("annotate add", handlers::annotate_add);
  table.emplace("annotate list", handlers::annotate_list);
  table.emplace("unlink", handlers::unlink);
  // `skills` has no subcommands, so it is a LEAF and needs an entry here
  // even though the verb is retired and does nothing but render its own
  // help page. See that handler's header.
  table.emplace("skills", handlers::skills);
  table.emplace("workspace doctor", handlers::workspace_doctor);
  table.emplace("workbench lint", handlers::workbench_lint);
  table.emplace("workbench pull", handlers::workbench_pull);
  table.emplace("workbench push", handlers::workbench_push);
  table.emplace("workbench status", handlers::workbench_status);
  table.emplace("workbench resolve", handlers::workbench_resolve);
  table.emplace("workbench sync", handlers::workbench_sync);
  table.emplace("workbench archive", handlers::workbench_archive);
  table.emplace("workbench restore", handlers::workbench_restore);
  table.emplace("workbench gc", handlers::workbench_gc);
  table.emplace("workbench list", handlers::workbench_list);
  return table;
}

auto unregistered_leaves(const CLI::App& root, const handler_table& table) -> std::vector<std::string> {
  std::vector<std::string> missing;
  for (auto& key : cliapp::leaf_keys(root)) {
    if (!table.contains(key)) {
      missing.push_back(std::move(key));
    }
  }
  return missing;
}

auto unreachable_handlers(const CLI::App& root, const handler_table& table) -> std::vector<std::string> {
  auto const                         keys = cliapp::leaf_keys(root);
  std::set<std::string, std::less<>> leaf_keys(keys.begin(), keys.end());
  std::vector<std::string>           dead;
  for (auto const& [key, unused] : table) {
    if (!leaf_keys.contains(key)) {
      dead.push_back(key);
    }
  }
  return dead;
}

auto run(context& ctx, CLI::App& root, const handler_table& table) -> int {
  auto const argv = ctx.argv();
  // CLI11's vector overload consumes argv[1..] in REVERSE order and never
  // sees argv[0] (see CLI::App::parse_char_t, which builds exactly this).
  std::vector<std::string> reversed;
  if (argv.size() > 1) {
    reversed.assign(argv.rbegin(), argv.rend() - 1);
  }

  try {
    root.parse(std::move(reversed));
  } catch (const CLI::CallForHelp&) {
    auto const [node, unused] = matched_node(root);
    ctx.out() << node->help();
    return exit_success;
  } catch (const CLI::ParseError& e) {
    // Both streams — see this module's header for the oracle capture and
    // for why task 6123 kept the shape while re-baselining the wording.
    ctx.out() << "error: " << e.what() << '\n';
    ctx.err() << "error: " << e.get_name() << '\n';
    // This binary's policy is exit 1, NOT the operator binary's 2.
    return exit_code_for(domain_error_kind::parse_error);
  }

  auto const [node, path] = matched_node(root);
  if (!cliapp::children(*node).empty()) {
    // A group named without a leaf beneath it — including a bare
    // `planar`, since this tree has no cockpit to route to.
    ctx.out() << node->help();
    return exit_success;
  }

  auto       args  = cliapp::harvest(root);
  auto const key   = cliapp::path_key(args.path);
  auto const found = table.find(key);
  if (found == table.end()) {
    auto const err = error_from_body(domain_error_kind::not_implemented, "not implemented yet");
    report(err, ctx.err());
    return exit_code(err);
  }

  auto const outcome = found->second(ctx, args);
  if (!outcome) {
    report(outcome.error(), ctx.err());
    return exit_code(outcome.error());
  }
  return exit_success;
}

} // namespace planar::cmd

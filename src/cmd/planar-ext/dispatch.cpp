/// @file dispatch.cpp
/// @brief Implementation of `planar.cmd.planar_ext.dispatch`.

module planar.cmd.planar_ext.dispatch;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cliapp.walk;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;
import planar.cmd.planar_ext.handler;
import planar.cmd.planar_ext.handlers.schema;
import planar.cmd.planar_ext.handlers.version;

namespace planar::cmd::ext {

namespace {

/// @brief The deepest node CLI11 actually matched, and the path taken to
/// reach it.
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

auto handlers(const CLI::App& root) -> handler_table {
  handler_table table;
  table.emplace("version", handlers::version);
  table.emplace("schema", [&root](context& ctx, const cliapp::parsed_args& args) -> handler_result {
    return handlers::schema(ctx, args, root);
  });
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
  auto const argv = cliapp::hoist_subcommands(root, ctx.argv());
  // CLI11's vector overload consumes argv[1..] in REVERSE order and never
  // sees argv[0].
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
    ctx.out() << "error: " << e.what() << '\n';
    ctx.err() << "error: " << e.get_name() << '\n';
    return exit_code_for(domain_error_kind::parse_error);
  }

  auto const [node, path] = matched_node(root);
  if (!cliapp::children(*node).empty()) {
    // A group named without a leaf beneath it, including a bare `planar-ext`.
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

} // namespace planar::cmd::ext

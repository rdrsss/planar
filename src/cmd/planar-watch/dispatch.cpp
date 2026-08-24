/// @file dispatch.cpp
/// @brief Implementation of `planar.cmd.planar_watch.dispatch`.

module planar.cmd.planar_watch.dispatch;

import std;
import cli11;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_watch.surface;
import planar.cliapp.walk;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.completion;
import planar.cmd.planar_watch.handlers.ledger;
import planar.cmd.planar_watch.handlers.live;
import planar.cmd.planar_watch.handlers.schema;
import planar.cmd.planar_watch.handlers.version;

namespace planar::cmd::watch {

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

/// @brief A handler that always refuses with `not_implemented` (exit 64),
/// naming the verb.
///
/// The loud refusal the full-surface declaration rests on (plan 996, task
/// 6065). Registered explicitly, per verb, rather than left to `run`'s
/// table-miss arm, so that a declared-but-unported leaf still satisfies the
/// `unregistered_leaves` gate — which is what keeps the generated inventory
/// honest as verbs get ported.
/// @param verb The root-relative path key, used verbatim in the message.
/// @return The handler.
auto not_implemented_for(std::string_view verb) -> handler_fn {
  return [body = std::format("{}: not implemented in this build", verb)](context&, const cliapp::parsed_args&) -> handler_result {
    return std::unexpected(error_from_body(domain_error_kind::not_implemented, body));
  };
}

auto handlers(const CLI::App& root) -> handler_table {
  handler_table table;
  // The six read verbs task 6120 landed. They take no `root`, unlike
  // `schema` and `completion` below, because they describe DATA rather than
  // the tree.
  table.emplace("ps", handlers::ps);
  table.emplace("claims", handlers::claims);
  table.emplace("actions", handlers::actions);
  table.emplace("plans", handlers::plans);
  table.emplace("log", handlers::log);
  table.emplace("tree", handlers::tree);
  table.emplace("version", handlers::version);
  table.emplace("schema", [&root](context& ctx, const cliapp::parsed_args& args) -> handler_result {
    return handlers::schema(ctx, args, root);
  });
  table.emplace("completion", [&root](context& ctx, const cliapp::parsed_args& args) -> handler_result {
    return handlers::completion(ctx, args, root);
  });
  // Everything above is IMPLEMENTED. Everything below is DECLARED and
  // refuses at exit 64; the inventory is generated alongside the surface
  // itself. `emplace` is a no-op on a key already present, so a stale
  // inventory entry cannot shadow a real handler.
  for (auto const& verb : unported_paths()) {
    table.emplace(std::string{verb}, not_implemented_for(verb));
  }
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

auto inject_default_verb(std::span<std::string const> argv) -> std::vector<std::string> {
  std::vector<std::string> out(argv.begin(), argv.end());
  if (out.size() <= 1) {
    // Just the binary name.
    out.emplace_back("feed");
    return out;
  }
  auto const& first = out[1];
  if (first == "--help" || first == "-h") {
    // Help requests stay as-is; the root's own help page is the answer.
    return out;
  }
  if (!first.empty() && first.front() == '-') {
    out.insert(out.begin() + 1, "feed");
  }
  return out;
}

auto run(context& ctx, CLI::App& root, const handler_table& table) -> int {
  auto const argv = cliapp::hoist_subcommands(root, inject_default_verb(ctx.argv()));
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
    // `planar-watch`, since `feed` is unported.
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

} // namespace planar::cmd::watch

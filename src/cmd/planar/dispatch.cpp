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
import planar.cmd.planar.surface;
import planar.cmd.planar.handlers.annotate;
import planar.cmd.planar.handlers.capture;
import planar.cmd.planar.handlers.catalog;
import planar.cmd.planar.handlers.handoff;
import planar.cmd.planar.handlers.resume;
import planar.cmd.planar.handlers.skills;
import planar.cmd.planar.handlers.ext;
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

/// @brief A handler that always refuses with `not_implemented` (exit 64),
/// naming the verb.
///
/// The loud refusal the full-surface declaration rests on. Registered
/// explicitly, per verb, rather than left to `run`'s table-miss arm,
/// because of two distinct hazards the arm does not cover: a DUAL node
/// (subcommands AND its own handler in the oracle) never reaches the arm at
/// all — it falls to the help page and exits 0, a silent success — and an
/// unregistered leaf trips the `unregistered_leaves` gate, which is what
/// keeps the generated inventory honest as verbs get ported.
/// @param verb The root-relative path key, used verbatim in the message.
/// @return The handler.
auto not_implemented_for(std::string_view verb) -> handler_fn {
  return [body = std::format("{}: not implemented in this build", verb)](context&, const cliapp::parsed_args&) -> handler_result {
    return std::unexpected(error_from_body(domain_error_kind::not_implemented, body));
  };
}

auto handlers(const CLI::App& root) -> handler_table {
  handler_table table;
  table.emplace("version", handlers::version);
  // `schema` and `completion` describe the TREE, so they take it; every
  // other handler describes DATA and does not. Same shape as the
  // `planar-agent` and `planar-watch` tables.
  table.emplace("schema", [&root](context& ctx, const cliapp::parsed_args& args) -> handler_result {
    return handlers::schema(ctx, args, root);
  });
  table.emplace("completion", [&root](context& ctx, const cliapp::parsed_args& args) -> handler_result {
    return handlers::completion(ctx, args, root);
  });
  table.emplace("workflow list", handlers::workflow_list);
  table.emplace("workflow show", handlers::workflow_show);
  table.emplace("annotate add", handlers::annotate_add);
  table.emplace("annotate list", handlers::annotate_list);
  table.emplace("unlink", handlers::unlink);
  table.emplace("ext register jira", handlers::ext_register_jira);
  table.emplace("ext register github", handlers::ext_register_github);
  table.emplace("ext list", handlers::ext_list);
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
  table.emplace("capture session", handlers::capture_session);
  table.emplace("capture end", handlers::capture_end);
  table.emplace("capture note", handlers::capture_note);
  table.emplace("capture command", handlers::capture_command);
  table.emplace("capture file", handlers::capture_file);
  table.emplace("capture snapshot", handlers::capture_snapshot);
  // `handoff` and `resume` are DUAL group-and-leaf nodes: each has
  // subcommands AND its own handler. Registering the parent is what makes
  // dispatch route the bare form to the handler instead of a help page.
  table.emplace("handoff", handlers::handoff);
  table.emplace("handoff create", handlers::handoff_create);
  table.emplace("handoff validate", handlers::handoff_validate);
  table.emplace("handoff consume", handlers::handoff_consume);
  table.emplace("handoff abandon", handlers::handoff_abandon);
  table.emplace("handoff list", handlers::handoff_list);
  table.emplace("handoff show", handlers::handoff_show);
  table.emplace("resume", handlers::resume_packet);
  table.emplace("resume validate", handlers::resume_validate);

  // Everything above is IMPLEMENTED. Everything below is DECLARED and
  // refuses at exit 64. The inventory is generated alongside the surface
  // itself (`planar.cmd.planar.surface`), so a verb that gains a real
  // handler above must be dropped from it in the same regeneration — the
  // `emplace` here is a no-op on a key already present, so a stale entry
  // cannot silently shadow a real handler, and `unported_paths` staying
  // stale in the other direction fails `unregistered_leaves`.
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
  // EVERY node, not only childless ones: a dual group-and-leaf node
  // (`handoff`, `resume`) carries a handler and is reachable through it.
  std::set<std::string, std::less<>> reachable;
  for (auto const& node : cliapp::all_nodes(root)) {
    reachable.insert(cliapp::path_key(node.path));
  }
  std::vector<std::string> dead;
  for (auto const& [key, unused] : table) {
    if (!reachable.contains(key)) {
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
  if (!cliapp::children(*node).empty() && !table.contains(cliapp::path_key(path))) {
    // A group named without a leaf beneath it AND with no handler of its
    // own — including a bare `planar`, since this tree has no cockpit to
    // route to. A group that DOES have a handler is dual (`handoff`,
    // `resume`) and falls through to it; see this module's header.
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

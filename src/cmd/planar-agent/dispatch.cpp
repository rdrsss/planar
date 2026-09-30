/// @file dispatch.cpp
/// @brief Implementation of `planar.cmd.planar_agent.dispatch`.

module planar.cmd.planar_agent.dispatch;

import std;
import cli11;
import planar.cmd.planar_agent.handlers.dispatch;
import planar.cmd.planar_agent.handlers.context;
import planar.cliapp.args;
import planar.cliapp.surface;
import planar.cmd.planar_agent.surface;
import planar.cliapp.walk;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.action;
import planar.cmd.planar_agent.handlers.claims;
import planar.cmd.planar_agent.handlers.queue;
import planar.cmd.planar_agent.handlers.recovery;
import planar.cmd.planar_agent.handlers.runs;
import planar.cmd.planar_agent.handlers.schema;
import planar.cmd.planar_agent.handlers.terminal;
import planar.cmd.planar_agent.handlers.version;

namespace planar::cmd::agent {

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
  table.emplace("version", handlers::version);
  table.emplace("schema", [&root](context& ctx, const cliapp::parsed_args& args) -> handler_result {
    return handlers::schema(ctx, args, root);
  });
  // The claim ritual. `unregistered_leaves` is what keeps this list honest
  // against `tree.cpp`: a verb added to the tree and forgotten here shows
  // up as an unwired leaf rather than as a `not implemented yet` an
  // operator discovers at runtime.
  table.emplace("pull", handlers::pull);
  table.emplace("peek", handlers::peek);
  table.emplace("claim", handlers::claim);
  table.emplace("heartbeat", handlers::heartbeat);
  table.emplace("claim-associate", handlers::claim_associate);
  table.emplace("complete", handlers::complete);
  table.emplace("fail", handlers::fail);
  table.emplace("release", handlers::release);
  table.emplace("block", handlers::block);
  table.emplace("action start", handlers::action_start);
  table.emplace("action end", handlers::action_end);
  table.emplace("reconcile", handlers::reconcile);
  table.emplace("abort", handlers::abort);
  table.emplace("dispatch preview", handlers::dispatch_preview);
  table.emplace("dispatch confirm", handlers::dispatch_confirm);
  table.emplace("context add", handlers::context_add);
  table.emplace("context capsule", handlers::context_capsule);
  table.emplace("context list", handlers::context_list);
  table.emplace("context resolve", handlers::context_resolve);
  table.emplace("ingest", handlers::ingest);
  table.emplace("run start", handlers::run_start);
  table.emplace("run end", handlers::run_end);
  table.emplace("run heartbeat", handlers::run_heartbeat);
  // The host-wide build and test queue (plan 1080). Its handler returns the
  // command's exit status, which dispatch passes through unchanged.
  table.emplace("queue run", handlers::queue_run);
  table.emplace("queue cancel", handlers::queue_cancel);
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

auto run(context& ctx, CLI::App& root, const handler_table& table) -> int {
  auto const argv = cliapp::hoist_subcommands(root, ctx.argv());
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
    // Decision 1004 (task 6271): every parse failure writes ONE line to
    // stderr, the same shape a handler refusal already writes — see this
    // module's header. The CamelCase tag (`e.get_name()`) is dropped
    // rather than kept as a second stderr line: keeping it would leave
    // parse failures as the only two-line refusal shape in the binary,
    // which is exactly the asymmetry this decision closes. Nothing is
    // written to stdout on a parse failure.
    ctx.err() << "error: " << e.what() << '\n';
    // This binary's policy is exit 1, NOT the operator binary's 2.
    return exit_code_for(domain_error_kind::parse_error);
  }

  auto const [node, path] = matched_node(root);
  if (!cliapp::children(*node).empty()) {
    // A group named without a leaf beneath it — a bare `planar-agent`,
    // or a bare `planar-agent action`.
    ctx.out() << node->help();
    return exit_success;
  }

  auto       args  = cliapp::harvest(root);
  auto const key   = cliapp::path_key(args.path);
  auto const found = table.find(key);
  // Task 6844 (decision 1145, supersedes D5): additive on stdout -- the
  // same stream a successful handler's JSON output already uses -- gated
  // on the same flag. report()'s pinned stderr text is unchanged.
  auto const want_json_envelope = cliapp::flag_bool(args, "--json");
  if (found == table.end()) {
    auto const err = error_from_body(domain_error_kind::not_implemented, "not implemented yet");
    report(err, ctx.err());
    if (want_json_envelope) {
      report_json_envelope(key, err, ctx.out());
    }
    return exit_code(err);
  }

  auto const handled = found->second(ctx, args);
  if (auto const* status = std::get_if<exit_status>(&handled)) {
    // Task 7007: the handler's exit code IS another process's status. It
    // is returned verbatim and never reported, even when it equals one of
    // this binary's own codes. A status no process can report without
    // truncation is a handler bug, refused with the internal-error code.
    if (status->code < exit_status_min || status->code > exit_status_max) {
      ctx.err() << std::format("error: {}: exit status {} is outside {}..{}\n", key, status->code, exit_status_min,
                               exit_status_max);
      return exit_internal_error;
    }
    return status->code;
  }
  auto const& outcome = std::get<handler_result>(handled);
  if (!outcome) {
    report(outcome.error(), ctx.err());
    if (want_json_envelope) {
      report_json_envelope(key, outcome.error(), ctx.out());
    }
    return exit_code(outcome.error());
  }
  return exit_success;
}

} // namespace planar::cmd::agent

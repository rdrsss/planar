/// @file dispatch.cpp
/// @brief Implementation of `planar.cmd.planar.dispatch`.

module planar.cmd.planar.dispatch;

import std;
import planar.cli;
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

/// @brief The Zig `Parse` error tag for a `parse_error_kind`.
///
/// main.zig writes `error: <@errorName(e)>` to stderr for every parse
/// failure, so these CamelCase tags are operator-visible bytes. Three were
/// captured directly (`UnknownSubcommand`, `UnknownFlag`,
/// `MissingRequiredPositional`); the rest are transcribed from
/// zig/vendor/etcli/src/cli/error.zig's `Parse` error set, whose members
/// map one-for-one onto this port's `parse_error_kind`.
///
/// `flag_group_violation` is a DELIBERATE divergence, called out rather
/// than hidden. It is a member of etcli's `Parse` set but has NO arm in
/// main.zig's switch, so on the Zig side it escapes `die` entirely and
/// propagates out of `main` as an unhandled error. Reproducing that is
/// neither possible nor desirable here, so it is mapped like every other
/// parse failure. It is unreachable through this task's verb subset —
/// none of the ported leaves declares a flag group — so nothing observable
/// changes; a later milestone that ports a flag-group-bearing leaf should
/// capture the oracle's actual behaviour before relying on this arm.
/// @param k The parse-error kind.
/// @return The Zig error tag.
auto zig_parse_error_name(cli::parse_error_kind k) -> std::string_view {
  switch (k) {
  case cli::parse_error_kind::unknown_flag:
    return "UnknownFlag";
  case cli::parse_error_kind::missing_value:
    return "MissingValue";
  case cli::parse_error_kind::invalid_value:
    return "InvalidValue";
  case cli::parse_error_kind::missing_required:
    return "MissingRequired";
  case cli::parse_error_kind::missing_required_positional:
    return "MissingRequiredPositional";
  case cli::parse_error_kind::too_many_positionals:
    return "TooManyPositionals";
  case cli::parse_error_kind::unknown_subcommand:
    return "UnknownSubcommand";
  case cli::parse_error_kind::unexpected_argument:
    return "UnexpectedArgument";
  case cli::parse_error_kind::duplicate_flag:
    return "DuplicateFlag";
  case cli::parse_error_kind::flag_group_violation:
    return "FlagGroupViolation";
  }
  return "UnknownParseError";
}

} // namespace

auto path_key(std::span<const std::string> path) -> std::string {
  std::string key;
  for (auto const& segment : path) {
    if (!key.empty()) {
      key += ' ';
    }
    key += segment;
  }
  return key;
}

auto handlers() -> handler_table {
  handler_table table;
  table.emplace("version", handlers::version);
  table.emplace("workflow list", handlers::workflow_list);
  table.emplace("workflow show", handlers::workflow_show);
  table.emplace("annotate add", handlers::annotate_add);
  table.emplace("annotate list", handlers::annotate_list);
  table.emplace("unlink", handlers::unlink);
  // `skills` has no subcommands, so `planar.cli.cmd` classifies it as a
  // leaf and it needs an entry here even though the verb is retired and
  // does nothing but render its own help page. See that handler's header.
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

auto unregistered_leaves(const cli::cmd& root, const handler_table& table) -> std::vector<std::string> {
  std::vector<std::string> missing;
  for (auto const& leaf : cli::all_leaves(root)) {
    auto key = path_key(leaf.path);
    if (!table.contains(key)) {
      missing.push_back(std::move(key));
    }
  }
  return missing;
}

auto unreachable_handlers(const cli::cmd& root, const handler_table& table) -> std::vector<std::string> {
  std::set<std::string, std::less<>> leaf_keys;
  for (auto const& leaf : cli::all_leaves(root)) {
    leaf_keys.insert(path_key(leaf.path));
  }
  std::vector<std::string> dead;
  for (auto const& [key, unused] : table) {
    if (!leaf_keys.contains(key)) {
      dead.push_back(key);
    }
  }
  return dead;
}

auto run(context& ctx, const cli::cmd& root, const handler_table& table) -> int {
  auto const argv   = ctx.argv();
  auto       parsed = cli::parse(root, argv);
  if (!parsed) {
    // Both streams — see this module's header for the oracle capture.
    ctx.out() << cli::format_error(parsed.error());
    ctx.err() << "error: " << zig_parse_error_name(parsed.error().kind) << '\n';
    return cli::exit_code_for_parse_error_planar_binary(parsed.error().kind);
  }

  if (parsed->is_help) {
    ctx.out() << cli::render_help(root, parsed->help_path);
    return cli::exit_success;
  }

  auto const key   = path_key(parsed->match.path);
  auto const found = table.find(key);
  if (found == table.end()) {
    // Only reachable for a tree leaf with no table entry, which
    // `unregistered_leaves` makes a test failure. Exit 64 matches
    // zig/src/cmd/planar/exit.zig's `error.NotImplemented => 64` and
    // main.zig's "not implemented yet" message.
    auto const err = error_from_body(cli::domain_error_kind::not_implemented, "not implemented yet");
    report(err, ctx.err());
    return exit_code(err);
  }

  auto const outcome = found->second(ctx, parsed->match);
  if (!outcome) {
    report(outcome.error(), ctx.err());
    return exit_code(outcome.error());
  }
  return cli::exit_success;
}

} // namespace planar::cmd

/// @file dispatch.cpp
/// @brief Implementation of `planar.cmd.planar_watch.dispatch`.

module planar.cmd.planar_watch.dispatch;

import std;
import planar.cli;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handler;
import planar.cmd.planar_watch.handlers.completion;
import planar.cmd.planar_watch.handlers.schema;
import planar.cmd.planar_watch.handlers.version;

namespace planar::cmd::watch {

namespace {

/// @brief The Zig `Parse` error tag for a `parse_error_kind`.
///
/// zig/src/cmd/planar-watch/main.zig writes `error: <@errorName(e)>` to
/// stderr for every parse failure. `UnknownSubcommand` and
/// `MissingRequiredPositional` were captured directly from this binary
/// (`planar-watch nosuchverb`, `planar-watch completion`); the rest are
/// transcribed from zig/vendor/etcli-zig/src/cli/error.zig's `Parse` set.
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

auto handlers(const cli::cmd& root) -> handler_table {
  handler_table table;
  table.emplace("version", handlers::version);
  table.emplace("schema", [&root](context& ctx, const cli::match_result& args) -> handler_result {
    return handlers::schema(ctx, args, root);
  });
  table.emplace("completion", [&root](context& ctx, const cli::match_result& args) -> handler_result {
    return handlers::completion(ctx, args, root);
  });
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
    ctx.out() << cli::format_error(parsed.error());
    ctx.err() << "error: " << zig_parse_error_name(parsed.error().kind) << '\n';
    // This binary's policy is exit 1, NOT the operator binary's 2 — see
    // this module's header for the capture.
    return cli::exit_code_for(cli::domain_error_kind::parse_error, cli::binary_kind::planar_agent);
  }

  if (parsed->is_help) {
    ctx.out() << cli::render_help(root, parsed->help_path);
    return cli::exit_success;
  }

  auto const key   = path_key(parsed->match.path);
  auto const found = table.find(key);
  if (found == table.end()) {
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

} // namespace planar::cmd::watch

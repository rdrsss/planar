/// @file handler.cppm
/// @brief `planar.cmd.planar.handler` — the one signature every verb
/// handler in this binary has (plan 996, task 6105).
///
/// Its own module, rather than a line in `dispatch.cppm`, purely so the
/// dependency runs one way: `dispatch` imports every handler module to
/// build its table, so a handler cannot import `dispatch` back. Both
/// import this.
///
/// The signature is `(context&, const cliapp::parsed_args&) ->
/// std::expected<void, domain_error>` — output goes to `ctx.out()`, failure
/// comes back as a value, and nothing calls `std::exit`. See
/// `planar.cmd.planar.exit`'s header for why the Zig original's `noreturn`
/// `die()` was not reproduced.
module;

export module planar.cmd.planar.handler;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace planar::cmd {

/// @brief What a handler returns: nothing on success, a typed failure
/// otherwise.
export using handler_result = std::expected<void, domain_error>;

/// @brief The erased handler type the dispatch table stores.
export using handler_fn = std::function<handler_result(context&, const cliapp::parsed_args&)>;

/// @brief Parse an entity-id positional the way the oracle's `plan`/`task`
/// verbs do (task 6141).
///
/// Ten leaves across the two families declare their id positional as a
/// STRING and parse it in the handler, so the refusal carries the oracle's
/// own wording (`plan id must be an integer, got 'abc'`) and its own exit
/// code (2, from `error.InvalidInput`) rather than a generic parser type
/// error at exit 1. Both shapes were captured from the oracle.
///
/// Shared here rather than copied per handler file: D19 exists because a
/// second copy of a rule drifts, and this one is a rule (the message, the
/// bucket, and the never-default-to-zero refusal), not plumbing.
/// @param args The parsed arguments.
/// @param positional The positional's declared name, e.g. `"plan-id"`.
/// @param label The noun for the message, e.g. `"plan"` — the oracle writes
/// `plan id`/`task id`, so this is the word before ` id`.
/// @return The id, or `invalid_input` (exit 2) for a non-integer or an
/// absent positional.
export auto entity_id_arg(const cliapp::parsed_args& args, std::string_view positional, std::string_view label)
    -> std::expected<std::int64_t, domain_error> {
  auto const raw = cliapp::positional_string(args, positional);
  if (!raw.has_value()) {
    // Unreachable through the CLI11 tree (every one of these positionals is
    // declared required), but an absent id must never fall through to 0 and
    // act on row 0 — the omitted-optional shape this port keeps closing.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("{} id is required", label)));
  }
  auto const parsed = cliapp::parse_int64_zig(*raw);
  if (!parsed.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("{} id must be an integer, got '{}'", label, *raw)));
  }
  return *parsed;
}

/// @brief The refusal for the declared-but-unimplementable `--touches`
/// filter on `plan list` / `task list` (task 6141).
///
/// `--touches <repo-slug>` restricts a listing to entities linked to that
/// repo through `entity_links … relationship='touches'`, served in the
/// oracle by `listTouching`. That query is not ported — the whole `task
/// touches` verb family is still in `unported_paths()` — and there is no
/// honest way to serve the flag without it.
///
/// The alternative, accepting the flag and ignoring it, is precisely the
/// defect this cycle's brief names in `planar-watch ps --vendor`: a filter
/// that silently does not filter returns plausible rows that no exit-code
/// or stdout diff catches. So it refuses at exit 64 — the same code every
/// unported verb uses — and names what has to land first.
/// @param verb The verb name to lead the message with, e.g. `"plan list"`.
/// @return The refusal.
export auto touches_not_implemented(std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::not_implemented,
                         std::format("{} --touches: not implemented in this build (the `touches` link surface is "
                                     "unported); re-run without --touches",
                                     verb));
}

} // namespace planar::cmd

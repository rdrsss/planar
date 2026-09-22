/// @file support.cppm
/// @brief `planar.cmd.planar_agent.handlers.support` — the failure-message
/// shapes every claim verb in this binary shares (plan 996, task 6038).
///
/// ## The stderr text is a contract, not a diagnostic
///
/// Zig's handlers all end the same way:
///
///     exit.die(ctx, e, "<verb>: {s}", .{@errorName(e)})
///
/// so the operator sees `error: complete: ClaimNotActive` and a script
/// greps for it. Every helper here reproduces one of those literal shapes.
/// The tags themselves come from `agentactivity::error_name`, which is why
/// they live down in the engine module rather than being re-spelled here:
/// one table, one place to be wrong.
///
/// ## Exit codes are NOT uniform across these messages
///
/// `planar-agent`'s `codeFor` has exactly four arms —
/// `InvalidEntityRef`/`InvalidInput` -> 2, the schema pair -> 7,
/// `NotImplemented` -> 64, and everything else -> 1. So EVERY store and
/// atomic failure exits 1, including the ones that read like user input:
///
///     planar-agent claim --ttl zzz            -> exit 1  (InvalidValue, unmapped)
///     planar-agent claim --entity bogus:1     -> exit 1  (UnsupportedEntityKind, unmapped)
///     planar-agent claim --entity task:abc    -> exit 2  (InvalidEntityRef)
///     planar-agent pull --metadata '{'        -> exit 2  (InvalidInput)
///
/// All four captured from the reference binary. The `--ttl` and
/// `--entity` rows are the surprising ones and they are why
/// `invalid_value_error` exists as a separate helper from
/// `invalid_input_error`: the messages read alike and the codes differ.
module;

export module planar.cmd.planar_agent.handlers.support;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.runtime.agentactivity;
import planar.engine.runtime.agentatomic;
import planar.engine.runtime.session;
import planar.cmd.planar_agent.exit;

namespace planar::cmd::agent {

/// @brief `error: <verb>: <Tag>` at exit 1 — the shape every store and
/// atomic failure takes.
/// @param verb The verb name as it appears in the message (e.g. `"complete"`).
/// @param err The engine failure.
/// @return The domain error to return from the handler.
export auto verb_error(std::string_view verb, engine::runtime::agentactivity::agent_error err) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure,
                         std::format("{}: {}", verb, engine::runtime::agentactivity::error_name(err)));
}

/// @brief `error: ensureActive: <Tag>` at exit 1.
///
/// The verb prefix is the Zig FUNCTION name, not the CLI verb —
/// `ensureActive`, camel-cased, in an otherwise snake-cased C++ tree.
/// That is what the oracle prints, so that is what this prints.
/// @param err The session failure.
/// @param aborting True for `abort`, whose prefix is `ensureActive (aborting)`.
/// @return The domain error to return from the handler.
export auto session_error_message(engine::runtime::session::session_error err, bool aborting = false) -> domain_error {
  std::string_view tag = "QueryFailed";
  switch (err) {
  case engine::runtime::session::session_error::not_found:
    tag = "NotFound";
    break;
  case engine::runtime::session::session_error::already_ended:
    tag = "AlreadyEnded";
    break;
  case engine::runtime::session::session_error::task_conflict:
    tag = "TaskConflict";
    break;
  case engine::runtime::session::session_error::query_failed:
    tag = "QueryFailed";
    break;
  }
  return error_from_body(domain_error_kind::generic_failure,
                         std::format("ensureActive{}: {}", aborting ? " (aborting)" : "", tag));
}

/// @brief A message that exits 2 — the `InvalidInput` bucket.
///
/// Used for the validations the handler performs itself before reaching
/// the engine: `--stage` without `--run`, malformed `--metadata`, an
/// out-of-range `--parent-action`, an oversized `--status`.
/// @param body The message body, no `error: ` prefix and no newline.
/// @return The domain error to return from the handler.
export auto invalid_input_error(std::string body) -> domain_error {
  return error_from_body(domain_error_kind::invalid_input, std::move(body));
}

/// @brief A message that exits 1 — the `InvalidValue` bucket.
///
/// Reads exactly like `invalid_input_error` and exits DIFFERENTLY. This is
/// the `--ttl` / `--stale-after` path: `cli.duration.parseSeconds` raises
/// `error.InvalidValue`, for which `planar-agent`'s `codeFor` has no arm,
/// so it falls through to the generic 1. Verified against the reference
/// binary (`planar-agent claim --entity task:1 --ttl zzz` exits 1).
/// @param body The message body, no `error: ` prefix and no newline.
/// @return The domain error to return from the handler.
export auto invalid_value_error(std::string body) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::move(body));
}

/// @brief The parsed engine-supervision flags (`--as`, `--attempt`,
/// `--override-supervisor`; plan 1033 task 6488). Owns the attempt text the
/// `supervisor_gate` built from it views.
export struct gate_flags {
  bool                       engine = false;              ///< `--as engine`.
  std::optional<std::string> attempt;                     ///< `--attempt`.
  bool                       override_supervisor = false; ///< `--override-supervisor`.

  /// @brief The engine gate over these flags; valid while `*this` lives.
  /// @return The gate.
  [[nodiscard]] auto gate() const -> engine::runtime::agentatomic::supervisor_gate {
    return engine::runtime::agentatomic::supervisor_gate{
        .as                  = engine ? engine::runtime::agentatomic::actor::engine : engine::runtime::agentatomic::actor::caller,
        .attempt             = attempt.has_value() ? std::optional<std::string_view>{*attempt} : std::nullopt,
        .override_supervisor = override_supervisor,
    };
  }
};

/// @brief Parse and cross-check the supervision flags, before anything is
/// written: `--as engine` needs `--attempt`, `--attempt` needs `--as
/// engine`, and `--override-supervisor` is the caller's (the engine never
/// overrides itself). Each refusal is invalid input, exit 2.
/// @param args The parsed arguments.
/// @return The flags, or the refusal.
export auto parse_gate_flags(const cliapp::parsed_args& args) -> std::expected<gate_flags, domain_error> {
  gate_flags out{
      .engine              = cliapp::flag_string(args, "--as") == std::optional<std::string>{"engine"},
      .attempt             = cliapp::flag_string(args, "--attempt"),
      .override_supervisor = cliapp::flag_bool(args, "--override-supervisor"),
  };
  if (out.engine && (!out.attempt.has_value() || out.attempt->empty())) {
    return std::unexpected(invalid_input_error("--as engine requires --attempt <id>"));
  }
  if (!out.engine && out.attempt.has_value()) {
    return std::unexpected(invalid_input_error("--attempt applies only with --as engine"));
  }
  if (out.engine && out.override_supervisor) {
    return std::unexpected(
        invalid_input_error("--override-supervisor is for the caller; it cannot be combined with --as engine"));
  }
  return out;
}

/// @brief The literal `--ttl` refusal text, shared by every verb that
/// takes a duration flag.
/// @param flag The flag name, e.g. `"--ttl"`.
/// @param raw The rejected value.
/// @param example The bare-seconds example this flag's message uses
/// (`"600"` for `--ttl`, `"0"` for `--stale-after`).
/// @return The domain error to return from the handler.
export auto duration_error(std::string_view flag, std::string_view raw, std::string_view example) -> domain_error {
  return invalid_value_error(std::format(
      "invalid {} '{}': expected bare seconds (e.g. {}) or suffixed duration (e.g. 10m, 1h, 500ms)", flag, raw, example));
}

} // namespace planar::cmd::agent

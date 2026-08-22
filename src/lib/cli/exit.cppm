/// @file exit.cppm
/// @brief `planar.cli.exit` — dispatch-level typed-error -> process-exit-
/// code mapping (task cpp-cli-output-logging).
///
/// error.cppm already exports the full numeric exit-code convention
/// (`exit_success` .. `exit_not_implemented`) and `exit_code_for
/// (parse_error_kind)`, which covers only the parser's own error set. A
/// real handler dispatch also raises domain errors that never touch the
/// parser (not-found, scope violations, slug conflicts, ...) — this module
/// adds `domain_error_kind`, the broader table mirroring
/// zig/src/cmd/planar/exit.zig's `codeFor`, and `exit_code_for(kind,
/// binary)` so a caller maps ONE typed error to the right code without
/// re-deriving the switch itself or duplicating error.cppm's constants.
///
/// Per-binary divergence (task 6063, planar-agent-parse-exit-divergence):
/// zig/src/cmd/planar/exit.zig maps every `cli.Parse.*` kind to exit 2
/// (documented AND implemented). zig/src/cmd/planar-agent/exit.zig's file
/// header *documents* the same 2, but its `codeFor` switch has no `Parse`
/// arm, so parse errors fall through to the generic `else => 1` bucket —
/// empirically verified (`./zig/zig-out/bin/planar-agent fail --reason x`
/// with no `--claim` -> exit 1, not 2). D2 binds this port to behavior
/// preservation, so `binary_kind::planar_agent` below reproduces the
/// ACTUAL (buggy-looking but real) exit-1 behavior, not the documented
/// exit-2 the Zig header claims. Task 6063 is the open item tracking
/// whether that divergence itself gets fixed upstream; until it resolves,
/// this module's two doc comments (here and on `exit_code_for`) must keep
/// agreeing with the code, unlike the Zig side's docs/code disagreement.
///
/// A SECOND, independent per-binary divergence (M2 boundary review, plan
/// 996 task 6066, found alongside the parse-error one above):
/// zig/src/runtime/runtime.zig:103 raises `error.SchemaVersionBehind` (live
/// DB older than the binary's embedded minimum — distinct from
/// `SchemaVersionAhead`, "stale binary", which both this module and the
/// Zig side already model). zig/src/cmd/planar-agent/exit.zig AND
/// zig/src/cmd/planar-watch/exit.zig both fold it into the SAME exit 7 as
/// `SchemaVersionAhead` (`error.SchemaVersionAhead, error.
/// SchemaVersionBehind => 7`). zig/src/cmd/planar/exit.zig's `codeFor` has
/// NO `SchemaVersionBehind` arm at all — only `SchemaVersionAhead => 7` —
/// so it silently falls through to the generic `else => 1` bucket. That is
/// a real THIRD divergence this module must not paper over: `domain_error_
/// kind::schema_version_behind` below reproduces exit 1 for `binary_kind::
/// planar` and exit 7 for `binary_kind::planar_agent` (which also stands
/// in for `planar_watch`'s identical policy — see `binary_kind`'s own doc
/// comment for why `planar_watch` has no dedicated enumerator here).
module;

export module planar.cli.exit;

import std;
import planar.cli.error;

namespace planar::cli {

/// @brief Which binary's exit-code policy to apply. Only the two binaries
/// task 6063 found diverging on `parse_error` are modeled as distinct
/// enumerators; `planar_watch` is read-only and raises no mutating-domain
/// errors, and `planar_execute` has no `schema`/dispatch surface of this
/// shape at all (see CLAUDE.md § five-binary boundary). `planar_watch`
/// DOES raise `SchemaVersionBehind`/`SchemaVersionAhead` at startup like
/// every other binary, but its policy for that pair is IDENTICAL to
/// `planar_agent`'s (both `=> 7` for both kinds — see this file's header
/// comment), so `binary_kind::planar_agent` stands in for it here; if
/// `planar_watch` ever gains a dispatch surface with its OWN divergent
/// policy, it earns its own enumerator then, not before.
export enum class binary_kind : std::uint8_t {
  planar,       ///< The operator binary — `cli.Parse.*` maps to exit 2.
  planar_agent, ///< The agent-callable binary — `cli.Parse.*` falls through to exit 1 (task 6063).
};

/// @brief The domain-error taxonomy dispatch handlers raise, mirroring
/// zig/src/cmd/planar/exit.zig's `codeFor` switch arms one-for-one (see
/// that file's exit-code convention table, reproduced here as this
/// module's contract):
///   0  success (not representable here — success has no error kind)
///   1  generic_failure / not_found (parity-triage §F-exit-code-not-found
///      folded NotFound back into the generic-1 bucket)
///   2  invalid_input / invalid_entity_ref / parse_error (`planar` only —
///      see `binary_kind`)
///   3  sync_conflict
///   5  scope_mismatch
///   6  slug_conflict / already_exists
///   7  schema_version_ahead
///   7  schema_version_behind (`planar_agent`/`planar_watch`) / 1
///      (`planar` — no arm, falls through to generic; see this file's
///      header comment, "SECOND, independent per-binary divergence")
///   64 not_implemented
export enum class domain_error_kind : std::uint8_t {
  generic_failure,
  not_found,
  invalid_input,
  invalid_entity_ref,
  parse_error,
  sync_conflict,
  scope_mismatch,
  slug_conflict,
  already_exists,
  schema_version_ahead,
  schema_version_behind,
  not_implemented,
};

/// @brief Map a `domain_error_kind` to the process exit code the given
/// binary's dispatch layer uses, applying the two per-binary divergences
/// documented in this file's header comment: `parse_error` (task 6063) and
/// `schema_version_behind` (plan 996 task 6066). Every other kind maps
/// identically for both binaries.
/// @param kind The domain-error kind a handler raised.
/// @param binary Which binary's exit-code policy to apply.
/// @return The process exit code (see `planar::cli`'s `exit_*` constants
/// in error.cppm for the named values this returns).
export auto exit_code_for(domain_error_kind kind, binary_kind binary) -> int {
  switch (kind) {
  case domain_error_kind::generic_failure:
  case domain_error_kind::not_found:
    return exit_generic_failure;
  case domain_error_kind::invalid_input:
  case domain_error_kind::invalid_entity_ref:
    return exit_user_input;
  case domain_error_kind::parse_error:
    return binary == binary_kind::planar ? exit_user_input : exit_generic_failure;
  case domain_error_kind::sync_conflict:
    return exit_sync_conflict;
  case domain_error_kind::scope_mismatch:
    return exit_scope_violation;
  case domain_error_kind::slug_conflict:
  case domain_error_kind::already_exists:
    return exit_precondition_conflict;
  case domain_error_kind::schema_version_ahead:
    return exit_schema_version_ahead;
  case domain_error_kind::schema_version_behind:
    // zig/src/cmd/planar-agent/exit.zig and zig/src/cmd/planar-watch/
    // exit.zig both fold SchemaVersionBehind into the same exit 7 as
    // SchemaVersionAhead. zig/src/cmd/planar/exit.zig has NO
    // SchemaVersionBehind arm — it silently falls through to the generic
    // `else => 1` bucket. Reproduced verbatim, not "fixed", per D2.
    return binary == binary_kind::planar ? exit_generic_failure : exit_schema_version_ahead;
  case domain_error_kind::not_implemented:
    return exit_not_implemented;
  }
  return exit_generic_failure;
}

} // namespace planar::cli

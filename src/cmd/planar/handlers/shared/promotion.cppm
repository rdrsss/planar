/// @file promotion.cppm
/// @brief `planar.cmd.planar.handlers.promotion` — the `planar promote` and
/// `planar demote` top-level leaves (plan 996, task 6299).
///
/// Port target: zig/src/cmd/planar/handlers/promote.zig (125 lines) and
/// handlers/demote.zig (121 lines). Both are pure wiring: the engine half —
/// `promote`, `demote`, `read_entity_scope` AND all three output renderers —
/// has been present as `planar.engine.promotion` since task 6094, which is
/// why these two leaves cost a handler each and no engine work at all.
///
/// ## The pre-read is what makes most of the engine's messages unreachable
///
/// Both handlers read the entity's scope BEFORE calling the engine, so they
/// can name the previous scope in the success line. That pre-read fails
/// first for every bad entity, which means the engine's own `not_found` and
/// `invalid_scope` messages (`no plan with id 999`, `unsupported entity kind
/// 'session'`) cannot be reached through the CLI at all. What an operator
/// actually sees is the pre-read's wording — `reading entity scope:
/// NotFound` — carrying the Zig `@errorName` tag verbatim. Both arms are
/// reproduced rather than simplified; see promotion.t.cpp for the captures.
///
/// ## `--from` is accepted and DISCARDED
///
/// `demote --from <slug>` parses and is then ignored: the oracle's engine
/// call takes no source scope and demotes unconditionally. `demote task:1
/// --from nonexistent-slug` therefore SUCCEEDS. Accepting-and-ignoring is
/// the behaviour under test, not a gap in this port.
///
/// ## Exit codes, oracle-captured
///
///   malformed ref (`plan`, `plan:`, `:1`)   exit 2  `invalid ref '<r>': expected kind:id`
///   UNKNOWN kind (`bogus:1`)                exit 2  same invalid-ref wording — `parse_ref`
///                                           refuses a kind it does not know
///   `plan:0` / `plan:-1`                    exit 2  also invalid-ref: `parse_ref` accepts
///                                           only POSITIVE integers, so a non-positive id is
///                                           a malformed ref rather than an absent entity
///   slug ref (`plan:some-slug`)             exit 2  `promote requires a numeric id (got slug '<r>')`
///   KNOWN but unpromotable kind (`session:1`)  exit 1  `reading entity scope: InvalidScope` —
///                                           `session` IS an `entity_kind`, so it survives
///                                           `parse_ref` and is refused one layer later by
///                                           the pre-read. Captured; the two bad-kind arms
///                                           land in DIFFERENT buckets (2 vs 1).
///   absent id                               exit 1  `reading entity scope: NotFound`
///   `--to repo:<slug>`                      exit 1  `repo: scopes are not supported`
///   `--to <unknown-slug>`                   exit 1  `no association with slug '<s>'`
///   already at the target scope             exit 1  `<kind>:<id> is already at scope '<s>'`
///   already global (demote)                 exit 1  `<kind>:<id> is already at global scope`
module;

export module planar.cmd.planar.handlers.promotion;

import std;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;
import cli11;

namespace planar::cmd::handlers {

/// @brief `planar promote <kind:id> --to <assoc-slug> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, or the refusal.
export auto promote(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar demote <kind:id> [--from <assoc-slug>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, or the refusal.
export auto demote(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `promote` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.

/// @brief Declare the `demote` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.

} // namespace planar::cmd::handlers

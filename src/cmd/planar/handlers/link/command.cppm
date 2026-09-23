/// @file src/cmd/planar/handlers/link/command.cppm
/// @brief `planar.cmd.planar.handlers.link` — the top-level `planar link
/// <kind:id> --to <system-slug>:<external-id>` leaf (plan 996, task 6301).
///
/// Port target: zig/src/cmd/planar/handlers/link.zig.
///
/// Distinct from `planar links add` (`handlers/links.cppm`), which writes
/// `entity_links` — intra-database edges. This verb writes `external_links`,
/// the same table `unlink` deletes from, and is its exact counterpart.
///
/// ## THE ORACLE'S OWN HEADER COMMENT ABOUT THIS FILE IS FALSE
///
/// `link.zig:9-11` says the `--propagate` flag is accepted "to keep CLI
/// parity but refuse[s] with NotImplemented when set". It does not. The code
/// writes the link row, prints the success line, and then runs FULL
/// propagation through `ext/propagate.zig`. Captured:
///
///     $Z link plan:1 --to jira-demo:DEMO-20 --propagate
///     linked plan:1 → jira-demo:DEMO-20  (link id: 8, read-only reference)
///     propagated plan 1 to jira-demo via strategy jira-epic
///       (created 0, skipped 1, failed 0)
///
/// This leaf was carried as engine-blocked for that reason and the belief
/// was documentary, not derived. It is the third time in three cycles that a
/// blocker turned out to rest on a claim about the code rather than on what
/// the code calls (`audit commits` and the `sync` trio were the other two).
/// A comment asserting a behaviour is a test that never runs.
///
/// ## THE `--propagate` DIVERGENCE, CHOSEN DELIBERATELY AND RECORDED HERE
///
/// Everything this verb needs is present — `external::link::create`,
/// `external::system::show_by_slug`, and the three `*_from_text` enums —
/// EXCEPT what `--propagate` reaches: `ext/propagate.zig`'s remaining
/// strategy/render/POST body and `anchorPlanFor`. `ext propagate` and `ext
/// propagate-one` are both still unported and land as their own cycle.
///
/// So this port serves the verb and REFUSES the one flag, at exit 64, on the
/// `touches_not_implemented` precedent (`handler.cppm`): a flag whose engine
/// is unported refuses loudly rather than being accepted and ignored,
/// because a `--propagate` that silently did not propagate is the defect
/// class that no exit code or stdout diff catches.
///
/// **The refusal fires BEFORE the link row is written**, and that placement
/// is the deliberate half of the divergence:
///
///   - The oracle writes the row and THEN propagates, so a "write, then
///     refuse" port would be no more faithful — the oracle itself leaves
///     partial state when propagation fails (`--propagate` on a task writes
///     the link, prints the line, then exits 1 with `AnchorPlanNotFound`).
///   - Refusing first is the only ordering under which the message's own
///     advice works. Had the row been written, the operator's re-run
///     without `--propagate` would hit the duplicate guard at exit 6, and
///     the refusal would have poisoned its own remedy.
///
/// Every OTHER path is byte-identical to the oracle.
///
/// ## THE TWO REFS PARSE IN OPPOSITE DIRECTIONS, IN ONE HANDLER
///
/// `<kind:id>` splits on the LAST colon; `--to <slug>:<external-id>` splits
/// on the FIRST, because an external id may itself contain colons while a
/// kind never does. They are not interchangeable and the asymmetry is the
/// oracle's.
///
/// ## IT VALIDATES THE KIND AND NOT THE ROW
///
/// `link task:999 --to jira-demo:DEMO-10` SUCCEEDS against an empty `tasks`
/// table — captured, exit 0, link id issued. The verb checks that the kind
/// is one of the seven `external_entity_kind` spellings and never asks
/// whether the entity exists, so a dangling link is reachable in one
/// command. Reproduced under D2 and pinned in `link_leaf.t.cpp`; it is an
/// oracle defect and needs a task row rather than a quiet fix here.
///
/// Note this makes `link` STRICTLY WIDER than `ext create`, which reads the
/// local row and therefore serves only four kinds: `link decision:1`
/// succeeds where `ext create --from decision:1` refuses.
///
/// ## THE CLI DEFAULTS ARE NOT THE ENGINE DEFAULTS
///
/// `create_args` defaults to `mirror` / `two-way` (the column defaults).
/// This verb passes `reference` / `read-only` EXPLICITLY, so an omitted
/// `--role` / `--sync` yields `(link id: N, read-only reference)`. Reading
/// the engine defaults as this verb's defaults would write a two-way mirror
/// where the operator asked for a read-only reference — a silent widening of
/// what may later be pushed to the remote.
///
/// It also differs from `ext create` on the initial status: this verb writes
/// `last_sync_status = 'never'` (nothing has been exchanged), where `ext
/// create` writes `'ok'` (it just created the counterpart).
///
/// ## `--scope` IS DECLARED AND DISCARDED
///
/// The literal `_ = args.scope;` in the oracle. `external_links` carries no
/// scope column and link verbs are UNGUARDED BY DESIGN
/// (docs/concepts.md § cross-scope-guard) — the same posture `unlink.cppm`
/// documents. It is named in the tree so `--help` matches and read nowhere.
///
/// ## NO AUDIT ROW, UNLIKE `unlink`
///
/// `unlink` appends a best-effort `session_entries` row; this verb writes
/// none. Confirmed against the oracle: `select count(*) from
/// session_entries` is 0 after ten successful links. The asymmetry is the
/// oracle's and adding one "for symmetry" would invent a trail entry no
/// reference binary emits.
module;

export module planar.cmd.planar.handlers.link;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar link <kind:id> --to <system-slug>:<external-id>
/// [--role <r>] [--sync <d>] [--propagate] [--scope <s>] [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success; `not_implemented` (exit 64) when `--propagate` is set;
/// `not_found` (exit 1) for an unknown system; `invalid_input` (exit 2) for
/// a malformed ref, a malformed `--to`, an unknown kind, or a bad `--role` /
/// `--sync`; or `slug_conflict` (exit 6) when the link already exists.
export auto link(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `link` leaf. Folded out of the generated
/// `surface.cpp` at M11.3f (task 6636, decision 1068).
/// @param root The root app to attach it to.
export auto declare_link(CLI::App& root) -> void;

} // namespace planar::cmd::handlers

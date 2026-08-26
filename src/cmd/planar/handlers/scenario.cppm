/// @file scenario.cppm
/// @brief `planar.cmd.planar.handlers.scenario` — the six `planar scenario`
/// CRUD/transition/link leaves (plan 996 roadmap M12 item 9, task 6195).
///
/// Port target: zig/src/cmd/planar/handlers/scenario/{add,show,list,verify,
/// retire,link}.zig.
///
/// ## The family is TEN leaves; six land here, four are named refusals
///
/// `scenario edit`, `scenario view`, `scenario diff` and `scenario review`
/// are the workbench DRAFTING quartet every planning entity carries. They
/// round-trip the row through a file under `$PLANAR_WORKBENCH_ROOT`, diff
/// the file against the row, and gate the re-import — `engine_workbench`
/// plumbing plus an editflow layer, neither ported. They stay declared
/// exit-64 refusals that name themselves, and `dispatch.t.cpp` asserts each
/// one INDIVIDUALLY rather than trusting the count.
///
/// ## These four are NOT broken in the oracle, unlike `decision`'s
///
/// Task 6195's brief said to expect the shape task 6194 found — `edit` and
/// `view` aborting with an unhandled `NotFound` and a Zig stack trace,
/// `diff` and `review` reporting `no X with id N` for a row that exists —
/// and to check whether scenario's behave the same. Ran all four: they do
/// NOT.
///
/// `scenario view 4`, `diff 4` and `review 4` on a scenario LINKED to a
/// plan all exit 0 and produce coherent output — a front-mattered workbench
/// document for `view`, a real unified diff against
/// `<root>/p1-anchor/scenarios/4-edge-with-body.md` for the other two.
/// `scenario edit 4` opens `$EDITOR` and blocks, which is the verb working.
/// Only an UNLINKED scenario (`scenario view 3`) hits the
/// `editflow.walkToAnchor` abort, and that is the anchor resolver having no
/// anchor to find rather than the verb being incoherent.
///
/// The difference is structural, not luck: `scenario add --plan` writes its
/// `entity_links` edge with `from_kind = 'test_scenario'`, which is exactly
/// the spelling `editflow.resolveAnchorPlan` queries. So these four DO have
/// a contract to port — the reason they are deferred is the missing
/// dependency alone, and the day `engine_workbench` and editflow land they
/// can be ported as-is rather than needing a decision first. That is a
/// weaker deferral than `decision`'s and is recorded as such deliberately.
///
/// ## `--status` and `--scope` on `scenario list` ARE comma-split
///
/// Both are, matching `question list` and `plan list` and NOT `decision
/// list`, whose identical-looking flags take one token each. Captured by
/// running them: `--status draft,verified` returns both, and `--scope
/// global,repo:oracle` resolves both slugs.
///
/// An unknown `--status` token exits **1** here, not 2: zig's list.zig dies
/// with `error.InvalidStatus`, which has no arm in `codeFor`. The
/// character-for-character identical refusal on `decision list` exits 2,
/// because THAT one raises `error.InvalidInput`. One flag name, three
/// verbs, two codes — and the flag specs cannot express any of it.
///
/// ## The empty `--status` filter means EVERY status
///
/// Including `retired`. `question`'s empty arm means `open`, `decision`'s
/// means `{proposed, accepted}` and `plan`'s meant "open" — the defect that
/// flipped done plans back to active. This family's answer was captured
/// rather than inherited: a bare `scenario list --scope global` over
/// draft/verified/retired rows returns all three.
///
/// ## `--touches` is SERVED here, not refused
///
/// `plan list --touches` and `task list --touches` refuse at exit 64
/// (`handler.cppm::touches_not_implemented`) because their `listTouching`
/// half was unported. `scenario`'s IS ported, as
/// `list_scenarios_touching`, so this leaf serves the flag for real. The
/// repo slug is resolved HERE and an unknown one REFUSES (`repo
/// 'nosuchrepo' not found`) rather than listing empty.
///
/// ## Where the scope comes from
///
/// `scenario add` cwd-derives its write scope when `--scope` is absent and
/// — like `question add` and `decision add`, unlike `plan create` — does
/// NOT refuse inside a project with no association: the row lands `global`.
///
/// `scenario list` cwd-derives a READ SET when `--scope` is absent and
/// refuses outright when the cwd is in no registered scope. An explicit
/// `--scope` REPLACES the set, filling the engine filter's VECTOR (as
/// `question list` does, and unlike `decision list`, which fills the
/// singular field).
///
/// `scenario verify`, `retire` and `show` declare no `--scope` at all;
/// `scenario link` declares one and ignores it, the same shared
/// entity-link behaviour every other `* link` arm has.
module;

export module planar.cmd.planar.handlers.scenario;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar scenario add <title> [--body --related --plan --scope --editor --json]`.
///
/// Unlike `decision add`, `--body` is genuinely optional: the column is
/// nullable and there is no refusal. `--editor` prints a warning and falls
/// through to the inline create — that warning is the ORACLE's own, not
/// this port's compensation, because the Zig verb has no editor flow
/// either.
///
/// This verb starts NO session and writes no `agent_actions` row, unlike
/// `question add` / `decision add`. `test_scenarios` has no `session_id`
/// column and zig's add.zig calls neither `ensureActive` nor the
/// entity-create activity hook.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scenario show <scenario-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scenario list [--scope --status --related --touches --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scenario verify <scenario-id> [--outcome --summary --json]`.
///
/// `--outcome` DEFAULTS TO `pass` when absent, which is what makes the
/// bare verb a "this passed" transition. An unrecognized value refuses at
/// exit 2 with `unknown outcome '<tok>' (want pass|fail|error|skipped)`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_verify(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scenario retire <scenario-id> [--reason --json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto scenario_retire(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar scenario link <scenario-id> <ref> --relationship <rel>
/// [--scope <s>] [--json]`.
///
/// One arm of the shared entity-link surface; the whole body lives in
/// `planar.cmd.planar.handlers.links::entity_link_verb`. Note the subject
/// kind is `test_scenario`, so the rendered line reads `linked
/// test_scenario:4 -> plan:1  [verifies]  (link id: 4)` even though the
/// VERB is spelled `scenario` and the `audit_log` entity kind this family
/// writes elsewhere is `scenario`. Three spellings, all captured.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, an
/// absent `--relationship`, an unknown relationship or a malformed ref, or
/// `generic_failure` (exit 1) for a duplicate link or a missing endpoint.
export auto scenario_link(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers

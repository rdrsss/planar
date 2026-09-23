/// @file src/cmd/planar/handlers/artifact/command.cppm
/// @brief `planar.cmd.planar.handlers.artifact` — the five `planar
/// artifact` CRUD/link leaves (plan 996 roadmap M12 item 10, task 6196).
///
/// Port target: zig/src/cmd/planar/handlers/artifact/{add,show,list,
/// update,link}.zig.
///
/// ## The family is NINE leaves; FIVE land here, four are named refusals
///
/// The dispatching brief for this task said to port "the artifact engine
/// and wire its nine leaves". Nine is the right leaf COUNT — `artifact
/// add | show | list | update | edit | view | diff | review | link`, and
/// `declare_artifact` below declares exactly those — but only five of
/// them reach an engine. `edit`, `view`, `diff` and `review` are the
/// workbench DRAFTING quartet, and they do not call
/// `planar.engine.planning.artifact` at all: each is a ~20-line zig
/// handler that forwards straight into `editflow`.
///
/// `engine_workbench` IS ported and all ten `workbench` leaves are wired,
/// so the deferral reason the `scenario` cycle recorded ("unported
/// `engine_workbench` plus editflow") is now only half true. What remains
/// is `zig/src/cmd/planar/editflow.zig` — 2076 lines of cmd-layer
/// anchor-resolution, front-matter round-trip and diff plumbing, plus
/// `editor.zig`'s 342 — and it is NOT artifact-specific: it gates the
/// same four leaves on `question`, `decision`, `scenario` and `artifact`
/// alike, SIXTEEN leaves in total. Porting it under a task scoped to "the
/// artifact engine" would have been a larger change than the engine
/// itself, landed without its own task row, and would have been
/// attributed to the wrong cycle. It stays out, the four stay declared
/// exit-64 refusals, and `dispatch.t.cpp` asserts each one INDIVIDUALLY
/// rather than trusting the count.
///
/// This is the fourth family in a row to defer the same quartet, and with
/// `artifact`'s engine landed the planning ENGINE surface is complete —
/// every remaining planning refusal is now blocked on that one cmd-layer
/// module and nothing else.
///
/// ## `--body @file` reads RAW; nothing strips front matter
///
/// The brief flagged a "body-wrapping subtlety" here — that the STRIPPED
/// body, not the front-matter-wrapped file, must reach the engine. Probed
/// directly: it does not work that way. `artifact update <id> --body
/// @<file>` stores the file BYTE FOR BYTE, leading `---` front-matter
/// block included, and so does `artifact add --body @<file>`. There is no
/// stripping anywhere on the `--body` path in the oracle;
/// `artifact.readBody` is a bare `readFileAlloc`. Front-matter parsing
/// lives in the workbench/editflow round-trip, which is a DIFFERENT path
/// and is one of the four leaves not ported here.
///
/// Implementing the stripping the brief described would therefore have
/// been a silent contract change, and a `--body @file` round-trip test
/// pins the raw bytes so the next cycle cannot reintroduce it.
///
/// ## Two refusal FORMATS from one family
///
/// `artifact show 999` reports the prose `no artifact with id 999`, while
/// `artifact list --scope nosuch` reports `artifact list: SlugNotFound` —
/// verb path plus the bare Zig tag. Both oracle-captured; the family
/// genuinely uses both shapes, split by whether the failure is a
/// single-id lookup.
///
/// ## `--kind` / `--status` validation refuses at exit 2
///
/// Both are comma-split, and an unknown token in either refuses the whole
/// call at exit 2 (`error: unknown kind 'nosuch'`). The
/// character-identical refusal on `scenario list --status` exits 1. The
/// validation runs in THIS layer, ahead of the engine, which is why the
/// engine's enums have no "unknown" member.
///
/// ## `artifact update` with no field is a PROSE refusal
///
/// `artifact update 1` with no mutable flag exits 1 with `at least one
/// field must be specified for update` — raised here, not by the engine,
/// which has its own `no_fields` arm the CLI never reaches.
///
/// ## `artifact add --editor` is silent, unlike `scenario add --editor`
///
/// `scenario add --editor` prints `warning: --editor not yet implemented;
/// falling back to inline create` and proceeds. `artifact add`'s
/// `--editor` defaults to TRUE in the surface and the oracle prints NO
/// warning at all — a bare `artifact add` with no `--body` simply creates
/// a row with a NULL body. Reproduced as-is; adding scenario's warning
/// here would put a line on stderr the oracle never writes.
module;

export module planar.cmd.planar.handlers.artifact;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar artifact add <title> --kind <k> [--body
/// --from-file --source-path --scope --status --plan --editor --json]`.
///
/// Scope is cwd-DERIVED when `--scope` is absent, and an unassociated
/// project is NOT a refusal — the row lands `global`, matching `question
/// add` / `decision add` / `scenario add` and unlike `plan create`.
///
/// `--plan` IS checked for existence by the engine, so a nonexistent one
/// refuses at exit 1 and writes nothing. `scenario add --plan` does not
/// check and leaves a dangling edge; the two families genuinely differ.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar artifact show <artifact-id> [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar artifact list [--kind --scope --status --plan --json]`.
///
/// The empty `--status` arm means `{draft, active}` — see
/// `engine/planning/artifact.cppm`. `--scope` fills the filter's VECTOR,
/// as `question list` and `scenario list` do.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar artifact update <artifact-id> [--title --body
/// --source-path --status --scope --json]`.
///
/// A `--status` move walks the transition matrix; see
/// `engine/planning/transitions.cppm`'s artifact arm.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_update(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar artifact link <artifact-id> <ref> --relationship
/// <rel> [--scope --json]`.
///
/// Forwards into the shared entity-link surface, like every other `* link`
/// arm. `--scope` is declared and ignored, which is that surface's
/// established behaviour rather than this family's quirk.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure to report.
export auto artifact_link(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `artifact` command tree on `root`.
///
/// The CLI declaration for every `artifact` node, colocated with the
/// handlers above (plan 1051, M11.3c — decision 1068). All ten came from
/// `surface.cpp`'s generated table; none was hand-declared in `tree.cpp`.
/// `edit`, `view`, `diff` and `review` are handled in
/// `handlers/drafting.cpp` and declared here with their domain, after
/// `update`, in catalog order.
/// @param root The root app to attach the `artifact` group to.
export auto declare_artifact(CLI::App& root) -> void;

} // namespace planar::cmd::handlers

/// @file workspace.cppm
/// @brief `planar.cmd.planar.handlers.workspace` — the `planar workspace
/// doctor` leaf (plan 996, task 6106).
///
/// Port target: zig/src/cmd/planar/handlers/workspace/doctor.zig.
///
/// ## This leaf was blocked on layer 3 and on nothing else
///
/// Task 6110 ported `engine_workspace` — org selection, the
/// `$PLANAR_HOME/workspaces/<id>/` layout, the diagnose-and-repair pass,
/// AND both renderers — and recorded in that bucket's CMakeLists that
/// `workspace init` was "architecturally blocked at layer 3". `doctor` was
/// blocked the same way and has been ready to wire ever since: everything
/// it needs is `doctor::run` plus `doctor_json` / `doctor_text`.
///
/// So this handler is deliberately THIN, and the thinness is the finding
/// rather than a shortcut. Contrast `unlink` in the same cycle, which had
/// to stand up a bucket first: "blocked on layer 3" covered two genuinely
/// different situations, and only this one was a pure wiring gap.
///
/// ## Doctor repairs while it diagnoses, and it writes where the DATABASE
/// says, not where the operator stands
///
/// Both facts belong to `planar.engine.workspace.doctor` and are argued in
/// full in that module's header; they are repeated here because they
/// change how this leaf must be TESTED. There is no `--dry-run`: asking
/// what is wrong creates the state directory and reinstalls the root
/// `AGENTS.md` / `CLAUDE.md` links. And the destination comes out of the
/// association's stored `config_json.root_path`, so isolating the working
/// directory protects nothing — only a scratch `$PLANAR_DB` does.
///
/// ## The two render modes disagree on an empty database, and both are right
///
/// `--json` emits `{"orgs":[]}` and a newline; the text mode emits ZERO
/// BYTES. Oracle-captured on both. That is the same per-renderer
/// terminator rule `workflow list --json` and `annotate list --json`
/// already split on, arriving here as a split between two modes of ONE
/// leaf: both renderers return COMPLETE payloads, so this handler writes
/// each verbatim and appends nothing to either.
///
/// ## `init`, `routing build`, `routing show`, `regenerate` are absent
///
/// Four of the five `workspace` children are NOT ported, so `planar
/// workspace --help` lists one command where the oracle lists four. That
/// follows the precedent `workflow` already set for `run` — an unported
/// child is OMITTED from the tree rather than half-wired — and it means
/// this GROUP's help page is not oracle-comparable while the LEAF's is.
/// `regenerate` and the `routing` pair are deferred at layer 2 (task
/// 6110's own note: size, and a missing xxh64); `init` is a 615-line
/// handler that composes scanning, enrichment and symlink installation and
/// is a cycle of its own.
module;

export module planar.cmd.planar.handlers.workspace;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief Handle `planar workspace doctor [--json]`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or `generic_failure` (exit 1) when the org listing
/// query fails.
export auto workspace_doctor(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Handle `planar workspace routing show [workspace] [--json]`.
///
/// ## Four failure paths, THREE different exit codes
///
/// All oracle-captured; the spread is the reason this leaf is not a
/// two-liner:
///
///     no org / unmatched slug   exit 1  no org associations registered; ...
///     two orgs, none named      exit 1  multiple org associations ...
///     routing-table.json absent exit 1  routing table not found at <path>;
///                                       run `planar workspace routing
///                                       build` first
///     file is not JSON          exit 1  decoding routing table failed:
///                                       SyntaxError
///     file is JSON, missing a   exit 2  decoding routing table failed:
///     required field                    InvalidInput
///
/// The last two share ONE message template and differ only in the
/// interpolated error tag, so a port that folded them together would move
/// an exit code while keeping every message byte identical. See
/// `planar.engine.workspace.routing`'s `decode_error`.
///
/// ## `--json` short-circuits before any of the decode failures
///
/// The JSON arm emits the file's bytes verbatim and never parses, so of the
/// five rows above it can only reach the first three. A file containing
/// `this is not json` exits 0 under `--json` and 1 without it. That is not
/// a bug to reconcile — it is the arm's whole definition.
///
/// ## It uses `load_layout`, NOT `ensure_layout`
///
/// Unlike `doctor` and `routing build`, `show` must not create the state
/// directory as a side effect of being asked to read from it. The Zig
/// original calls `loadLayout` for exactly that reason and this port
/// preserves it, so a `show` against a workspace that was never built
/// leaves the filesystem untouched and refuses.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal matching one of the rows above.
export auto workspace_routing_show(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers

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
/// ## `init` and `regenerate` are still absent
///
/// Two of the five `workspace` children are NOT ported. `regenerate` is
/// blocked on an unvendored xxh64 (its `.manifest-docs` merkle is keyed by
/// it) plus a hand-rolled template engine; `init` is a 615-line handler
/// that COMPOSES scan + registration + routing build + regenerate + symlink
/// install, which decision 947 places at layer 3.
///
/// `routing build` left this list at task 6275, and with it went the note
/// that the `routing` pair was "deferred at layer 2 ... size". The sizing
/// was right and the pairing was not: `show` came out at task 6110 because
/// it only needed a decoder, and `build` came out here because size was
/// genuinely the only thing in its way.
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
///     two orgs, none named      exit 2  multiple org associations ...
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
/// ## The AMBIGUOUS row above is a CORRECTION (task 6275)
///
/// It read `exit 1` until task 6275, and the oracle answers 2. `show.zig`
/// and `build.zig` refuse through the identical
/// `exit.die(ctx, error.InvalidInput, "multiple org associations
/// registered; pass the workspace slug or id explicitly", .{})`, so the
/// error is `InvalidInput` and lands in the user-input bucket.
///
/// Captured side by side, two orgs registered and no selector given:
///
///     zig/zig-out/bin/planar workspace routing show   exit 2
///     build/debug/bin/planar workspace routing show   exit 1   <-- wrong
///
/// with stderr byte-identical on both. That is exactly the shape this
/// file's own paragraph above warns about — one message, two codes — and it
/// was missed because `resolve_error::ambiguous` was mapped to
/// `generic_failure` while the message it pairs with was correct. Nothing
/// that asserts on the message can see it; only an exit-code assertion can,
/// and there now is one.
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

/// @brief Handle `planar workspace routing build [workspace] [--enrich] [--json]`.
///
/// ## It WRITES, and it writes where the DATABASE says
///
/// Unlike `show`, this leaf uses `ensure_layout`: the state directory is
/// created as a side effect of building. And both the directory and every
/// scanned checkout come out of the database — `$PLANAR_HOME/workspaces/
/// <org_id>/` for the output, and each member's recorded `root_path` for the
/// input. Redirecting the working directory protects nothing; a scratch
/// `PLANAR_DB` is the isolation that matters, exactly as for `doctor`.
///
/// ## Two files it reads, and the asymmetry between them
///
/// `$PLANAR_HOME/templates/workspace-capabilities.toml` REPLACES the
/// built-in capability rules wholesale, and `<state-dir>/
/// routing-table-overrides.json` merges over the built table. Both are
/// optional. The asymmetry: an EMPTY rules file (or one whose every line
/// precedes the first `[[rule]]` header) yields zero rules and falls back to
/// the defaults, while empty overrides simply change nothing. Oracle-captured
/// on both.
///
/// ## `--enrich` is an accepted no-op, and its warning goes to STDOUT
///
/// The oracle prints `warning: --enrich is not yet implemented in Zig;
/// skipping enrichment pass` on STDOUT — not stderr — and only when `--json`
/// is absent. `enrich_enabled` and `enrich_misses` are hardcoded `false` and
/// `0` in the JSON arm regardless. All three captured. Reproduced as-is
/// rather than refused: the flag parses and the build proceeds.
///
/// Note the warning precedes the `built ...` line, so the text arm emits TWO
/// lines under `--enrich` and one without it.
///
/// ## Failure paths
///
///     no org / unmatched slug   exit 1  no org associations registered; ...
///     two orgs, none named      exit 2  multiple org associations ...
///     malformed rules TOML      exit 1  loading capability rules failed:
///                                       ParseFailed
///     unquoted rules scalar     exit 2  loading capability rules failed:
///                                       InvalidInput
///     overrides not JSON        exit 1  loading routing overrides failed:
///                                       SyntaxError
///     overrides not an object   exit 2  loading routing overrides failed:
///                                       InvalidInput
///
/// Four of the six share two message templates across two exit codes each,
/// for the same reason `show`'s decode failures do.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or one of the refusals above.
export auto workspace_routing_build(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::handlers

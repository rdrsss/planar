/// @file propagate.cppm
/// @brief `planar.cmd.planar_ext.handlers.propagate` — `planar ext propagate
/// <plan-id> [--system <slug>] [--dry-run] [--sync <dir>] [--json]`, the
/// GitHub `parent-issue` arm only (plan 996, task 6421).
///
/// Port target: `zig/src/cmd/planar/handlers/ext/propagate.zig` (1007
/// lines). This module ports the `github-parent-issue` short-circuit
/// (`runParentIssueStrategy`, ~380 of those lines) plus the strategy
/// resolution and output rendering the happy path needs. It does NOT port:
///
///   - `--restrategize` / `--yes` / `--verify-counterparts` / `--unlink` /
///     `--recreate` / `--github-strategy` — the "M10 engine path" flags.
///     Every one of them needs the strategy-stickiness cache
///     (`external_links.config_json` read/write, `abandonCounterparts`,
///     `listMirrorLinksInTree`, `recordCounterpartMissing`), none of which
///     is ported. Passing any of them is simply not offered by this
///     binary's CLI surface yet — a follow-up task, not a silent
///     acceptance-and-ignore.
///   - The generic per-entity tree walk (`jira-epic`, `github-zero-repo`,
///     `github-tracking-issue` — every strategy that is not
///     `github-parent-issue`). `ext propagate-one` already has this body
///     (`propagate_one_entity`'s equivalent lives inline in `ext.cpp`); a
///     follow-up task should lift it into a shared loop the way the
///     oracle's own `propagate.zig` does at its `for (tree) |entry|` arm.
///     Until then, a feature whose selected strategy is anything but
///     `github-parent-issue` refuses explicitly rather than mis-executing.
///   - `github-projects-v2` execution — decision 1001 cut this arm from the
///     C++ rewrite entirely. `select_strategy` still REPORTS this bucket
///     name for a >=2-repo GitHub feature (see `ext_strategy.cppm`); this
///     module is the layer that turns that name into an explicit refusal
///     rather than either executing it (impossible, unported) or silently
///     downgrading the feature to `github-parent-issue` (wrong: a
///     multi-repo feature crammed into a single-repo strategy would pick an
///     arbitrary first repo and mis-attribute every other repo's entities).
///
/// ## THE `selectStrategy`-WITH-ONE-CHOICE QUESTION, ANSWERED
///
/// The task brief asked whether a chooser with one remaining choice is
/// worth porting at all, or whether this bridge should call
/// `parent_issue::propagate_parent_issue` directly. Answer: port
/// `select_strategy` and CALL it, because it is not actually a
/// one-choice chooser even after the projects_v2 cut — it still
/// discriminates `jira` vs `github-issues`, and within `github-issues` it
/// still discriminates 0 vs 1 vs >=2 touched repos. Calling
/// `propagate_parent_issue` unconditionally would be WRONG on two of those
/// branches: a Jira system has no parent-issue concept at all, and a
/// >=2-repo GitHub feature would have `resolve_target_repo` silently pick
/// the first touched repo (see that function's own doc comment: "returns
/// coordinates for the first repo") and mis-attribute every entity outside
/// it. `select_strategy` is what lets this module tell those cases apart
/// and refuse them BY NAME instead of mis-executing them.
module;

export module planar.cmd.planar_ext.handlers.propagate;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.handler;

namespace planar::cmd::ext::handlers {

/// @brief Handle `planar ext propagate <plan-id> [--system <slug>]
/// [--dry-run] [--sync <dir>] [--json]`.
///
/// Resolves the plan, resolves the target system, selects the ADR-0006
/// strategy, and — only when that strategy is `github-parent-issue` — runs
/// `engine::external::parent_issue::propagate_parent_issue` through a
/// production `gh_client` wired over the built `adapter_handle`. Every
/// other selected strategy refuses explicitly; see this module's header.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the failure.
export auto ext_propagate(context& ctx, const cliapp::parsed_args& args) -> handler_result;

} // namespace planar::cmd::ext::handlers

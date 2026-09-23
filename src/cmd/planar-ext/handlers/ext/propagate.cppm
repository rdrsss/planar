/// @file propagate.cppm
/// @brief `planar.cmd.planar_ext.handlers.propagate` — `planar ext propagate
/// <plan-id> [--system <slug>] [--dry-run] [--sync <dir>]
/// [--restrategize [--yes]] [--verify-counterparts [--unlink | --recreate]]
/// [--scope <slug>] [--json]`, the GitHub `parent-issue` arm only (plan
/// 996, task 6421; the strategy-stickiness flags landed at task 6428).
///
/// Port target: `zig/src/cmd/planar/handlers/ext/propagate.zig` (1007
/// lines). This module ports the `github-parent-issue` short-circuit
/// (`runParentIssueStrategy`, ~380 of those lines), the strategy
/// resolution and output rendering the happy path needs, AND (task 6428)
/// the `--restrategize`/`--yes`/`--verify-counterparts`/`--unlink`/
/// `--recreate`/`--scope` preflight and post-pass — see `propagate.cpp`'s
/// `ext_propagate` for exactly how each is wired. It still does NOT port:
///
///   - `--github-strategy` — decision 1001 cut the `projects-v2` arm it
///     would select PERMANENTLY from the C++ rewrite, and `tracking-issue`
///     is not ported either (see the next bullet). Accepting the flag only
///     to refuse every value it could carry adds a flag with no reachable
///     accepting case, which is worse than not declaring it. The oracle's
///     own doc comment on this flag is authored assuming `projects-v2` is
///     still live; that assumption does not hold in this tree. Authored
///     surfaces (skills, docs, agents) were swept to remove every
///     `--github-strategy` reference at task 6428 rather than leaving it
///     as stale prose.
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
///
/// ## `--restrategize` IS A FAITHFUL PORT THAT IS PRACTICALLY INERT HERE,
/// AND THAT IS HONEST, NOT A SHORTCUT
///
/// `propagate_parent_issue` writes `"strategy":"github-parent-issue"` into
/// the anchor's `config_json` on its very first successful link, and this
/// binary can only ever REACH `--restrategize`'s comparison after
/// `select_strategy` has already resolved to `github-parent-issue` (a Jira
/// system or a multi-repo GitHub feature refuses earlier, before the
/// restrategize block runs — see `ext_propagate`). So the cached value and
/// the freshly-selected value are always the same string in this binary,
/// and the abandon path never fires. That is not a bug this port papers
/// over: it is the correct consequence of `projects-v2`/`tracking-issue`
/// being unreachable, and the general-purpose comparison is still the
/// right thing to run — it is what makes the day this binary gains a
/// second executable strategy a one-line change rather than a rewrite.
///
/// ## `--scope` IS DECLARED AND DISCARDED, MATCHING THE ORACLE EXACTLY
///
/// The oracle's handler reads `_ = args.scope;` — the flag is parsed and
/// thrown away. `external_links` carries no scope column, and `ext
/// propagate` is a bulk-write-from-parent verb over that unguarded table
/// (docs/concepts.md § cross-scope-guard: link verbs are UNGUARDED BY
/// DESIGN). This module reproduces the discard rather than inventing a
/// guard the oracle never runs.
module;

export module planar.cmd.planar_ext.handlers.propagate;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.handler;

namespace planar::cmd::ext::handlers {

/// @brief Handle `planar ext propagate <plan-id> [--system <slug>]
/// [--dry-run] [--sync <dir>] [--restrategize [--yes]]
/// [--verify-counterparts [--unlink | --recreate]] [--scope <slug>]
/// [--json]`.
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

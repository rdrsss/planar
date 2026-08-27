/// @file editflow.cppm
/// @brief `planar.cmd.planar.editflow` — the `edit | view | diff | review`
/// drafting quartet shared by four planning families (plan 996, task 6205).
///
/// Ported from `zig/src/cmd/planar/editflow.zig` (2076 lines). ONE module
/// gates SIXTEEN leaves — `question`, `decision`, `scenario` and `artifact`
/// each declare the same four — which is why it is a task of its own rather
/// than a rider on any one family's engine port.
///
/// It is a CMD-layer helper, a peer of `context` and `exit`, NOT an engine
/// bucket and NOT under `handlers/`. That placement is inherited from the
/// Zig original and is correct for the same reason: it composes
/// `engine_workbench` with raw per-entity SQL that no engine surface
/// exposes, and every one of the sixteen handlers is a five-line shim over
/// it.
///
/// ## WHAT THE ORACLE ACTUALLY DOES, captured by RUNNING it
///
/// Every statement below was derived by running `zig/zig-out/bin/planar`
/// over a pinned scratch arena (`PLANAR_DB`, `PLANAR_HOME`,
/// `PLANAR_CONFIG_PATH`, `PLANAR_LOCAL_HOME`, `PLANAR_WORKBENCH_ROOT` and
/// `HOME` all redirected), not by reading zig.
///
/// ### The four families AGREE — and the brief for this task said they would not
///
/// The task brief warned that "the four families disagree, and have
/// disagreed every single time", citing `scenario`'s quartet working where
/// `decision`'s is broken, on the theory that `scenario add --plan` writes
/// `from_kind = 'test_scenario'` — the spelling the anchor resolver queries
/// — while the others do not.
///
/// That is NOT what the database contains. After one `add --plan 1` per
/// family, `entity_links` holds:
///
///     artifact|1|plan|1|derives-from
///     decision|1|plan|1|derives-from
///     question|1|plan|1|derives-from
///     test_scenario|1|plan|1|derives-from
///
/// All four rows are present. `scenario` alone spells its `from_kind`
/// `test_scenario`, and `entity_link_kind` ALREADY maps it — so
/// the resolver finds all four. Run against a plan-linked row, all sixteen
/// leaves behave identically; run against an UNLINKED row, all sixteen fail
/// identically. The observed `scenario`-works/`decision`-breaks split was a
/// LINKED row compared against an UNLINKED one, not a family difference.
///
/// This is recorded at length because the previous four cycles each found a
/// real per-family divergence and this one genuinely does not. The
/// divergences that DO exist are between VERBS, and they are below.
///
/// ### `view`/`edit` and `diff`/`review` use DIFFERENT RENDERERS
///
/// Not a subtlety — it is why `diff` reports changes on a file `view` wrote
/// one second earlier.
///
///   `view`/`edit`   `render_entity` in THIS file: a thin body carrying
///                   title and status only.
///   `diff`/`review` `engine.workbench.sync::render_entity`: the CANONICAL
///                   renderer, which also carries `**Created:**` and
///                   `**Updated:**` and, for `scenario`, the body text.
///
/// So `planar question view 1 && planar question diff 1` prints a diff
/// deleting the two timestamp lines the canonical form has and the thin one
/// does not. Oracle-confirmed on all four families; reproduced verbatim.
///
/// ### `view`/`edit` and `diff`/`review` disagree about WHERE the file LIVES,
/// ### and only for `artifact`
///
///   question   `<feature>/questions/<id>-<slug>.md`   both paths agree
///   decision   `<feature>/decisions/<id>-<slug>.md`   both paths agree
///   scenario   `<feature>/scenarios/<id>-<slug>.md`   both paths agree
///   artifact   view/edit:    `<feature>/artifacts/<id>-<slug>.md`
///              diff/review:  `<feature>/<id>-<slug>.md`   <-- feature ROOT
///
/// The consequence is observable and is NOT cosmetic: `artifact view 1`
/// writes a file under `artifacts/`, and `artifact diff 1` then compares
/// the database against the feature ROOT — which is empty — and prints the
/// entire rendered document as a deletion hunk (`@@ -1,18 +0,0 @@`). Both
/// shapes ship. `dispatch.cpp` and `handlers/artifact.cppm` already record
/// this divergence from the artifact cycle; this port preserves it rather
/// than unifying the two, because unifying it would change what
/// `workbench push` subsequently reports as drift.
///
/// ### `view`/`edit` do NOT map their failures; `diff`/`review` do
///
/// On an entity with no `derives-from` edge:
///
///   `question diff 2`    exit 1, `error: question 2 is not linked to a
///                        plan; cannot resolve anchor plan`
///   `question review 2`  exit 1, the same line
///   `question view 2`    exit 1, `error: NoPlanLink` FOLLOWED BY a Zig
///                        stack trace — seven frames of absolute paths into
///                        the oracle's own build tree.
///
/// THE ONE DELIBERATE DIVERGENCE IN THIS PORT is the trace. This build
/// emits the first line and the exit code — `error: NoPlanLink`, exit 1 —
/// and stops. Reproducing the frames is not possible (they are file
/// offsets into a Zig binary) and not desirable (the paths name a tree D6
/// deletes at M10). It is called out here, in `dispatch.cpp` and in the
/// test file rather than left for a reader to discover from a diff.
///
/// Note also that the id is never checked for EXISTENCE first: `question
/// view 999` and `question diff 999` report `NoPlanLink` and `is not linked
/// to a plan`, NOT `no question with id 999`, because the resolver queries
/// `entity_links` before it queries the entity. The `not_found` arm in the
/// sixteen handlers is therefore unreachable for these four families. It is
/// kept, because it is what the oracle's handlers declare, and dropping it
/// would be a silent narrowing of the contract.
///
/// ### `diff` and `review` are the SAME verb when no verdict is given
///
/// `review <id>` with neither `--approve` nor `--request-changes` prints
/// byte-identical output to `diff <id>`. It diverges only with `--json`
/// (a summary envelope, `verdict: null`) or with a verdict.
///
/// ### Equal content is SILENT
///
/// `diff` on a file matching the database writes nothing and exits 0,
/// mirroring `diff -u`. A caller cannot distinguish "no changes" from "did
/// not run" by stdout alone, which is why the tests assert the file bytes.
///
/// ### "title and status ONLY" still passes THREE gates
///
/// The reduced flow does not write arbitrary status text. In order:
///   1. `engine.workbench.parse` rejects a status outside the PER-KIND set,
///      as `InvalidFieldValue`, before this module sees it.
///   2. `scenario` — and only `scenario` — runs the transition matrix.
///   3. The TABLE's own CHECK constraints, which nothing here consults.
///
/// Gate 3 bites: `questions` requires `answer_body` and `answered_at` for
/// `status = 'answered'`, and this flow writes neither, so a front-matter
/// edit to `answered` is refused by SQLite as a bare `error: QueryFailed`
/// naming neither the constraint nor `question answer`. Oracle-confirmed on
/// the first stderr line and reproduced rather than improved.
export module planar.cmd.planar.editflow;

import std;
import planar.db;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;

namespace planar::cmd {

/// @brief The six planning entity kinds the quartet covers.
///
/// `plan` and `task` are declared because the flow genuinely handles them —
/// `resolve_anchor_plan` and `entity_rel_path` both have live arms for them
/// — but their eight leaves are NOT wired by this task. See this module's
/// implementation notes and `dispatch.cpp`.
export enum class entity_kind : std::uint8_t {
  plan,
  task,
  question,
  scenario,
  decision,
  artifact,
};

/// @brief The kind's name in front matter and workbench paths.
/// @param kind The entity kind.
/// @return The name.
export auto entity_kind_name(entity_kind kind) -> std::string_view;

/// @brief The kind's `entity_links.from_kind` spelling.
///
/// Identical to `entity_kind_name` for five of six. `scenario` is stored as
/// `test_scenario`, and that ONE mapping is why the quartet resolves an
/// anchor for scenarios at all.
/// @param kind The entity kind.
/// @return The link kind.
export auto entity_link_kind(entity_kind kind) -> std::string_view;

/// @brief The explicit decision `--approve` / `--request-changes` carries.
export enum class review_verdict : std::uint8_t {
  approve,
  request_changes,
};

/// @brief Optional overrides for `edit`.
export struct edit_opts {
  /// @brief When set, bypasses the `PLANAR_EDITOR`/`VISUAL`/`EDITOR` chain.
  std::optional<std::string> editor_override;
};

/// @brief Render the entity to its canonical workbench file, then open that
/// file in `$PAGER` -> `less` -> `cat`.
///
/// WRITES THE FILE AS A SIDE EFFECT, and that is the operator-visible half:
/// the pager inherits stdout, so what reaches the terminal is produced by a
/// child process, not by this binary. Does not touch the database.
/// @param ctx The invocation context, for the environment and streams.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @return Nothing, or the failure.
export auto view(context& ctx, db::connection& conn, entity_kind kind, std::int64_t id) -> std::expected<void, domain_error>;

/// @brief Render the entity, open it in `$EDITOR`, and apply the title and
/// status the operator saved.
///
/// TITLE AND STATUS ONLY. Every other front-matter field is compared, and a
/// change to any of them produces the oracle's literal `[M4 limitation: ...]`
/// line on stderr and is then DROPPED. That is the Zig original's
/// documented reduced flow, not an omission in this port.
///
/// A non-zero editor exit aborts without writing, as does a save that
/// changed neither title nor status.
/// @param ctx The invocation context.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @param opts The overrides.
/// @return Nothing, or the failure.
export auto edit(context& ctx, db::connection& conn, entity_kind kind, std::int64_t id, const edit_opts& opts)
    -> std::expected<void, domain_error>;

/// @brief Write a unified diff of the CANONICAL render against the current
/// workbench file. Equal content writes nothing.
/// @param ctx The invocation context.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @return Nothing, or the failure.
export auto diff(context& ctx, db::connection& conn, entity_kind kind, std::int64_t id) -> std::expected<void, domain_error>;

/// @brief Report the database-versus-workbench state.
///
/// With no verdict this is `diff` in text form, or a preview envelope with
/// `verdict: null` in JSON form. With a verdict it emits a stable summary.
/// Nothing is persisted in either case — there is no per-entity review
/// table in the schema, and the envelope says so.
/// @param ctx The invocation context.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @param verdict The explicit verdict, if one was given.
/// @param json Whether to emit the JSON envelope.
/// @return Nothing, or the failure.
export auto review(context& ctx, db::connection& conn, entity_kind kind, std::int64_t id, std::optional<review_verdict> verdict,
                   bool json) -> std::expected<void, domain_error>;

/// @brief Walk from an entity to the top-level plan that anchors it.
///
/// `plan` walks `parent_plan_id`; `task` reads `tasks.plan_id` and then
/// walks; the four link-reached kinds query `entity_links` for a
/// `derives-from` edge to a plan and then walk.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @return The anchor plan id, or the failure.
export auto resolve_anchor_plan(db::connection& conn, entity_kind kind, std::int64_t id)
    -> std::expected<std::int64_t, domain_error>;

/// @brief The absolute path `view` and `edit` render into.
///
/// Exposed so tests can assert the file the flow WROTE rather than the
/// bytes a pager child happened to print. See this module's header for why
/// this and the `diff`/`review` path differ for `artifact`.
/// @param ctx The invocation context, for the workbench root.
/// @param conn The database connection.
/// @param kind The entity kind.
/// @param id The entity id.
/// @param anchor_id The resolved anchor plan.
/// @return The absolute path, or the failure.
export auto view_path(const context& ctx, db::connection& conn, entity_kind kind, std::int64_t id, std::int64_t anchor_id)
    -> std::expected<std::string, domain_error>;

} // namespace planar::cmd

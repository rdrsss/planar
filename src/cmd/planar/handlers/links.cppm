/// @file links.cppm
/// @brief `planar.cmd.planar.handlers.links` — the four `planar links`
/// leaves plus the shared arm behind `plan link`, `task link` and
/// `question link` (plan 996 roadmap M12 item 7, task 6193).
///
/// Port target: zig/src/cmd/planar/handlers/links/{add,list,remove,
/// trail}.zig and zig/src/cmd/planar/handlers/{plan,task,question}/link.zig.
///
/// ## Seven leaves, one surface, deliberately landed together
///
/// `engine_entitylink` was fully ported and exported no renderer, which is
/// the single reason all seven refused at exit 64. Task 6188 declined to
/// wire `question link` on its own precisely because doing so would make
/// `planar question link 1 plan:2` work while `planar plan link 2 plan:1`
/// refused — one arm of a shared surface, which reads as a bug rather than
/// a milestone boundary. All seven land here.
///
/// `links update` is NOT one of them and is not a cut: it is HIDDEN in the
/// oracle (`handlers/links/update.zig` is a documented stub deferred to
/// M11) and is not among the four `links` leaves the surface declares.
/// The `ext` and `sync` families are a genuine cut — they render
/// `external_links`, a different table with its own envelopes, and share
/// nothing with this module but the word "link".
///
/// ## Three JSON envelopes over one table
///
/// Captured by running the oracle, not by reading it. All three coexist:
///
///   - `links list`   `{"id":…,"from_kind":…,…,"created_at":…}` — the ROW,
///                    no `ok`, WITH the stamp. NDJSON, one per line.
///   - `links add`    `{"ok":true,"id":…,"from_kind":…,…}` — `ok`, NO stamp.
///   - `<entity> link` `{"ok":true,"id":…,"plan_id":…,"to_kind":…,…}` — the
///                    subject collapses into a PER-VERB key (`plan_id` /
///                    `task_id` / `question_id`) and the from-side columns
///                    disappear entirely.
///
/// ## The empty `--json` case is ZERO BYTES
///
/// `links list` and `links trail` both emit NOTHING at all — not `[]`, not
/// `\n` — when there is nothing to list, and still exit 0. That is why the
/// engine's `render_link_list_json` / `render_trail_json` own their
/// terminators instead of following `engine_planning`'s
/// caller-terminates-the-fragment contract; under that contract the empty
/// case would emit a stray newline. The TEXT empty case is a line
/// (`no links for task:999`), so the two halves diverge and each was
/// captured separately.
///
/// ## `plan link` spells its arrow differently from its two siblings
///
/// The `already exists` refusal is `link plan:1 → task:1 [depends-on]
/// already exists` on `plan link` (U+2192) and `link task:1 -> plan:2 …`
/// on `task link`, `question link` and `links add`. Confirmed by running
/// all four and by `handlers/plan/link.zig`'s literal `\u{2192}` against
/// its siblings' `->`. It is an oracle inconsistency, reproduced (D2), and
/// the engine renderer takes the arrow as a REQUIRED parameter so no call
/// site can inherit the wrong one by default.
///
/// ## `--scope` is accepted and INERT here, on purpose
///
/// Link verbs are UNGUARDED BY DESIGN (CLAUDE.md § cross-scope-guard:
/// "Link verbs … are deliberately unguarded" — they create `entity_links`
/// edges that may legitimately cross scopes). The three `<entity> link`
/// leaves declare `--scope` and the oracle ignores its value completely:
/// `plan link 1 task:4 --relationship cites --scope nosuchscope` SUCCEEDS
/// and writes the link, exactly as `--scope global` does. Verified by
/// running both. This is NOT the `annotate sweep --scope` defect class
/// (an inert filter on a destructive bulk mutation): nothing here is
/// filtered, the flag never narrowed anything in the oracle either, and
/// `engine_entitylink` refuses a non-null scope outright
/// (`unsupported_scope`) rather than resolving it. `resolve_write_scope` is
/// deliberately NOT called — calling it would ADD a guard the oracle does
/// not have.
///
/// ## `already exists` is exit 1, not exit 6
///
/// The obvious mapping — `domain_error_kind::already_exists`, the
/// precondition-conflict bucket — is exit 6 and would be WRONG. The oracle
/// exits 1 on a duplicate link, so `link_exists` lands in the generic
/// bucket. Captured, not inferred.
module;

export module planar.cmd.planar.handlers.links;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.entitylink;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar links add <from-ref> <to-ref> --relationship <rel>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, a typed failure otherwise.
export auto links_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar links list <ref>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, a typed failure otherwise.
export auto links_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar links remove <link-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, a typed failure otherwise.
export auto links_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar links trail <link-id>`.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Nothing on success, a typed failure otherwise.
export auto links_trail(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief The whole body of `plan link`, `task link` and `question link`.
///
/// Shared rather than copied three times: the three differ only in their
/// subject kind, the noun in their id-parse refusal, their JSON key and —
/// for `plan link` alone — the arrow in their duplicate refusal. Every
/// other byte, the ordering of the four refusal checks included, is
/// identical. D19's rule applies: a second copy of a rule drifts, and the
/// arrow inconsistency above is exactly what drift looks like.
///
/// Ordering is load-bearing and oracle-captured: subject id parses FIRST
/// (so `plan link notanint task:2` reports the plan id even with
/// `--relationship` absent), then `--relationship` presence, then its
/// value, then the ref, then endpoint existence.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param subject_kind The subject's entity kind.
/// @param id_positional The subject id positional's declared name, e.g. `"plan-id"`.
/// @param id_label The noun for the id-parse refusal, e.g. `"plan"`.
/// @param json_key The subject's JSON key, e.g. `"plan_id"`.
/// @param verb The verb name for engine-error messages, e.g. `"plan link"`.
/// @param unicode_arrow True only for `plan link`; see this module's header.
/// @return Nothing on success, a typed failure otherwise.
export auto entity_link_verb(context& ctx, const cliapp::parsed_args& args, engine::entitylink::entity_kind subject_kind,
                             std::string_view id_positional, std::string_view id_label, std::string_view json_key,
                             std::string_view verb, bool unicode_arrow) -> handler_result;

/// @brief Declare the `links` command tree on `root`.
///
/// The CLI declaration for every `links` node, colocated with the
/// `links_*` handlers above (plan 1051, M11.3e — decision 1068). None was
/// hand-declared in `tree.cpp`.
///
/// Distinct from the top-level `link` / `unlink` leaves, which live in
/// `handlers/link.cpp` and `handlers/unlink.cpp` and address EXTERNAL
/// ticket linkage; this group addresses internal `entity_links` rows. The
/// two are neighbours in the catalog (`link` 15th, `unlink` 16th, `links`
/// 17th) and the group's own description says so.
/// @param root The root app to attach the `links` group to.
export auto declare_links(CLI::App& root) -> void;

} // namespace planar::cmd::handlers

/// @file src/cmd/planar/handlers/models/command.cppm
/// @brief `planar.cmd.planar.handlers.models` — all fourteen `models` leaves
/// (plan 996, tasks 6149 and 6343).
///
/// The ten `models registry` leaves plus `evals`, `experiments`, `outcomes`
/// and `resolve`. `resolve` is the family's last leaf, wired at task 6343
/// once `engine_ingest`'s planning half (`assemble_planning`,
/// `compile_planning`) landed alongside `engine_models`'s `profile` and
/// `roles` (task 6111). See `models_resolve`'s own doc comment for the
/// two-branch shape (task-bound vs. planning) and the adapter it owns:
/// `engine::ingest::packet::evidence` -> `engine::models::profile::fact`,
/// which is this handler's job per `src/lib/engine/models/CMakeLists.txt`'s
/// note on where that seam belongs.
///
/// "Fourteen" is the LEAF count and is correct as written (verified, task
/// 6648): ten `models registry` leaves plus `evals`, `experiments`,
/// `outcomes`, `resolve`. A count of 15 shows up if the two GROUP nodes
/// (`models`, `models registry`) are counted alongside the leaves they
/// contain — those are not leaves, so 15 is the wrong number here. Do not
/// "fix" this header to 15 without re-deriving the leaf/group split first.
///
/// ## `models evals` is TWO verbs sharing a name, and `--vendor` picks
///
/// The single most consequential thing in this file, and it was PROBED
/// rather than inferred. A NON-EMPTY `--vendor` selects the evidence-backed
/// cohort ranking; anything else — including `--vendor ""` — falls through
/// to the legacy note-convention scorecard. Every other cohort flag is
/// INERT on its own:
///
///     $Z models evals --vendor v        -> exit 2, "--project is required
///                                          when ranking a cohort"
///     $Z models evals --role r          -> exit 0, LEGACY scorecard
///     $Z models evals --tier medium     -> exit 0, LEGACY scorecard
///     $Z models evals --project 1       -> exit 0, LEGACY scorecard
///     $Z models evals --min-samples 3   -> exit 0, LEGACY scorecard
///     $Z models evals --vendor ""       -> exit 0, LEGACY scorecard
///
/// A port that triggered the cohort branch on "any cohort flag present"
/// would turn six ordinary legacy invocations into exit-2 refusals, and a
/// port that triggered on "`--vendor` present" rather than "non-empty"
/// would refuse `--vendor ""`. Both exit 0 on the reference binary.
///
/// ## The cohort branch's validation ORDER is observable and is pinned
///
/// Nine checks, and each one was isolated by a probe that satisfies every
/// earlier check and violates exactly one:
///
///     1. --project           required   ("--project is required when ranking a cohort")
///     2. --min-samples       parseable  ("invalid --min-samples '<v>'")
///     3. --quality-floor     parseable  ("invalid --quality-floor '<v>'")
///     4. --validation-policy required
///     5. --routing-policy    required
///     6. --role              required
///     7. --tier              required, then valid ("invalid --tier '<v>'")
///     8. --work-type         required, then valid
///     9. --complexity        required, then valid
///
/// Every one of these exits 2 with a one-line stderr message and NO stdout,
/// so a test that checks only the exit code cannot tell them apart — which
/// is why each is pinned by its bytes.
///
/// ## Two leaves where "no output" is the correct answer
///
///     $Z models registry list     (empty registry) -> exit 0, ZERO BYTES
///     $Z models registry bind ... (success)        -> exit 0, ZERO BYTES
///
/// Neither is an error and neither prints a confirmation. `registry list
/// --json` on the same empty registry prints the full envelope with an
/// empty `candidates` array, so the two wire formats genuinely disagree
/// about the empty case.
///
/// ## `models registry export` writes to BOTH streams, and `--json` mutes one
///
/// Oracle-captured, and the asymmetry is the trap:
///
///     $Z models registry export          stdout: the JSON envelope
///                                        stderr: "warning: legacy catalog
///                                                 compatibility is one-window
///                                                 and non-authoritative"
///     $Z models registry export --json   stdout: the SAME envelope
///                                        stderr: EMPTY
///
/// The stdout payload is byte-identical either way — `--json` on this leaf
/// controls the STDERR warning alone, the reverse of every other leaf in
/// the family.
///
/// ## The error prefixes are per-leaf and are NOT interchangeable
///
/// Six distinct prefixes over one `registry_error`, all captured:
///
///     add              "registering opaque candidate: Conflict"     exit 3
///     update / remove  "updating|removing candidate: NotFound"      exit 1
///     bind             "binding candidate: QueryFailed"             exit 1
///     observe          "recording host observation: Conflict"       exit 3
///     eligibility      "reading candidate: NotFound"                exit 1
///     verify-identity  "candidate <id> not found"                   exit 1
///
/// Note `bind` and `observe` against a NONEXISTENT candidate disagree —
/// `bind` reports `QueryFailed` at exit 1, `observe` reports `Conflict` at
/// exit 3, from the same missing row. Both are FK failures the engine
/// classifies differently, and `registry.cppm` documents `bind`'s as
/// deliberate. And `verify-identity` alone does not use the
/// `<gerund>: <ErrorName>` shape at all.
module;

export module planar.cmd.planar.handlers.models;

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

/// @brief `planar models registry list` — every registration with its
/// bindings and newest observation.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_list(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry export` — the same envelope `list --json`
/// prints, plus a stderr warning that `--json` mutes.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_export(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry add` — register one opaque candidate.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_add(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry update` — enabled state and fallback order.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_update(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry remove` — drop a candidate no immutable
/// evidence references.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry bind` — allow one role and tier.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_bind(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry unbind` — remove one role/tier binding.
/// Succeeds silently on a binding that never existed.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_unbind(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry observe` — append a versioned host
/// capability observation.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_observe(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry eligibility` — the six independent gates
/// and the named reason for each failure.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_eligibility(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models registry verify-identity` — exact comparison of
/// requested and actual spawn identity, with no aliasing.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_registry_verify_identity(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models evals` — the cohort ranking when `--vendor` is
/// non-empty, the legacy note-convention scorecard otherwise. See this
/// module's header.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_evals(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models experiments` — declared experiments with recorded
/// and eligible sample counts (which differ whenever a sample was excluded).
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_experiments(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models outcomes` — recorded terminal outcomes, newest
/// first, capped by `--limit` (default 50).
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_outcomes(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief `planar models resolve` — resolve one role's routing tier from its
/// authoritative packet, or report the configured static fallback and why.
///
/// Task-bound roles (`coder`, `test-coder`, `reviewer`, `research`,
/// `janitor`) resolve from `--task`'s compiled task packet; pre-task roles
/// (`planner`, `spec-reviewer`, `ingestor`, `orchestrator`) resolve from
/// `--plan`'s compiled planning packet, or from no packet at all when
/// `--plan` is omitted.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @return Success, or the refusal.
export auto models_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result;

/// @brief Declare the `models` command tree on `root`.
///
/// The CLI declaration for every `models` node, colocated with the handlers
/// above (plan 1051, M11.3b — decision 1068's one-declaration-site-per-node
/// target, sited per task 6401 next to the handler rather than in a second
/// central file). `planar.cmd.planar.tree` calls this while building the
/// root app.
///
/// The order of the `add_subcommand` calls IS the order the `--help` page
/// and the `schema` catalog list the children in — the ONE invariant this
/// hand declaration carries that the compiler cannot check. `models` is the
/// only domain in this wave with a depth-3 group (`models registry`), whose
/// ten children are declared by a file-local helper.
/// @param root The root app to attach the `models` group to.
export auto declare_models(CLI::App& root) -> void;

} // namespace planar::cmd::handlers

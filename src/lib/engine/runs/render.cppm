/// @file render.cppm
/// @brief `planar.engine.runs.render` — the validation and output-rendering
/// halves of the nine ported `bench` / `run` schema leaves (plan 996, task
/// 6095).
///
/// Behavior-preserving port (D2) of zig/src/cmd/planar/handlers/bench/*.zig
/// and zig/src/cmd/planar/handlers/run/*.zig — everything in those handlers
/// except the runtime/context plumbing, which has no home until a `cmd_*`
/// layer exists.
///
/// ORACLE PROVENANCE. Every string below was captured by RUNNING the Zig
/// binary against a scratch database, never from `--help` and never from
/// reading the Zig source. render.t.cpp's header carries the verbatim probe
/// transcript.
///
/// ## Two surfaces over one shape
///
/// `bench show` and `run show` render the SAME run from the SAME table but
/// are not the same renderer, and the differences are not cosmetic:
///
///   - `bench show --json` carries `base_sha`, `config_hash`, `config_json`,
///     `corpus_repo`, and a `touches` array. `run show --json` carries NONE
///     of them — touches are measurement-only and deliberately omitted from
///     the operational surface.
///   - The text renderers pad their labels to DIFFERENT widths (`bench` to
///     13, `run` to 12), so the two are not one parameterized function.
///
/// ## Raw-JSON embedding
///
/// `config_json` and `payload` are stored as raw text but emitted VERBATIM
/// into the enclosing object, not as escaped strings. Oracle-captured:
///
///   $Z bench start r3 ... --config-json '{"k":1}'
///   $Z bench show r3 --json
///       -> ..."config_json":{"k":1},...        [an object, not a string]
///   $Z bench event r1 --kind result --seq 2 --payload '{"a":1}'
///       -> ..."payload":{"a":1},...
///
/// This is why both are validated as well-formed JSON at the parse layer
/// (`is_valid_json_payload` below) — an invalid blob would produce invalid
/// output downstream, so it is refused up front at exit 2.

module;

export module planar.engine.runs.render;

import std;
import planar.engine.runs.lifecycle;

namespace planar::engine::runs::render {

/// @brief The experiment arms `bench start` recognizes.
///
/// This is a RECOGNIZED set, not a closed one. An unrecognized arm is
/// accepted — the oracle warns on stderr and still exits 0:
///
///   $Z bench start r1 --plan 1 --arm a1 ...
///       stderr: warn: bench start: unrecognized arm 'a1'; recognized arms:
///               strict, eligibility, grouped
///
/// (In that capture the command then failed for an unrelated reason — the
/// plan did not exist — but the warn is emitted before the insert is
/// attempted and a valid plan yields exit 0 with the same warn.)
export inline constexpr std::array<std::string_view, 3> k_known_arms{"strict", "eligibility", "grouped"};

/// @brief The terminal statuses `bench finish` / `run finish` accept.
///
/// Unlike the arms this IS a closed set: an out-of-set value is refused at
/// exit 2. Note it is NOT the same vocabulary as the `runs.status` column's
/// default (`running`) nor as `workflow_runs`' CHECK — `ok` and `failed` are
/// both REFUSED, which the oracle confirms.
export inline constexpr std::array<std::string_view, 3> k_terminal_statuses{"completed", "aborted", "error"};

/// @brief Whether `arm` is one of the three recognized experiment arms.
/// @param arm The candidate arm text.
/// @return `true` when recognized; `false` means "warn but proceed".
export auto is_known_arm(std::string_view arm) -> bool;

/// @brief Whether `status` is an accepted terminal status.
/// @param status The candidate status text.
/// @return `true` when accepted; `false` means "refuse at exit 2".
export auto is_valid_terminal_status(std::string_view status) -> bool;

/// @brief Whether `blob` parses as a single well-formed JSON value with no
/// trailing content.
///
/// Used for both `--config-json` and `--payload`. Shares the trailing-content
/// strictness engine/config/templates.cpp settled on in task 6086:
/// `{"a":1} junk` is NOT valid (oracle-confirmed, exit 2). Surrounding
/// whitespace IS valid and is preserved verbatim on the way back out —
/// `--payload '   {"a":1}   '` round-trips to
/// `"payload":   {"a":1}   ,` in `bench show --json`. Any bare JSON value
/// is accepted, not just objects: `123`, `null`, and `"str"` all pass. The
/// empty string does not.
/// @param blob The candidate JSON text.
/// @return `true` when the whole input is exactly one JSON value.
export auto is_valid_json_payload(std::string_view blob) -> bool;

/// @brief The `warn:` line `bench start` writes to stderr for an
/// unrecognized arm. Oracle-captured verbatim.
/// @param arm The unrecognized arm.
/// @return The warn line, WITHOUT a trailing newline.
export auto render_unknown_arm_warning(std::string_view arm) -> std::string;

/// @brief The `run '<uid>' not found` error message, prefixed by the leaf.
///
/// Every one of the six uid-taking leaves shares this wording, differing only
/// in the prefix — oracle-captured for `bench show`, `bench event`,
/// `bench touch`, `bench finish`, `run show`, and `run finish`.
/// @param leaf The leaf name, e.g. `"bench show"`.
/// @param run_uid The uid that was not found.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_run_not_found(std::string_view leaf, std::string_view run_uid) -> std::string;

/// @brief The invalid-`--status` refusal, prefixed by the leaf.
/// @param leaf The leaf name, e.g. `"bench finish"`.
/// @param status The rejected status.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_invalid_status(std::string_view leaf, std::string_view status) -> std::string;

/// @brief The invalid-`--kind` refusal for `bench touch`.
/// @param kind The rejected kind.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_invalid_touch_kind(std::string_view kind) -> std::string;

/// @brief The invalid-`--task` refusal for `bench start`'s REPEATABLE
/// task filter (plan 996, task 6149).
///
/// Distinct from every other integer flag in this family and deliberately
/// so. `bench touch --task` is declared `(int)` on the tree, so CLI11's
/// own validator rejects a non-integer at parse time with the parser's
/// wording; `bench start --task` is declared `(string)` + `list` because
/// it is repeatable, so the tree accepts ANY text and the leaf must do the
/// conversion — and report it in the oracle's words rather than the
/// parser's. Oracle-captured:
///
///   $Z bench start bt2 --plan 1 --arm strict --base-sha s --config-hash c \
///                      --task notanint
///     exit 2, stderr b"error: bench start: --task value must be an
///                      integer, got 'notanint'\n"
///
/// A port that let the value fall through as 0 would snapshot NOTHING and
/// still exit 0 — no stdout difference at all, since the leaf prints only
/// the uid.
/// @param raw The rejected value, echoed verbatim.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_invalid_task_id(std::string_view raw) -> std::string;

/// @brief The invalid-JSON refusal for `--payload` / `--config-json`.
///
/// Note the flag name is part of the message and the offending blob is echoed
/// in full.
/// @param leaf The leaf name, e.g. `"bench event"`.
/// @param flag The flag name including dashes, e.g. `"--payload"`.
/// @param blob The rejected blob, echoed verbatim.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_invalid_json(std::string_view leaf, std::string_view flag, std::string_view blob) -> std::string;

/// @brief The duplicate-`run_uid` refusal for `bench start`.
/// @param run_uid The colliding uid.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_duplicate_run_uid(std::string_view run_uid) -> std::string;

/// @brief The duplicate-`seq` refusal for `bench event`.
/// @param seq The seq already taken.
/// @param run_uid The run it is taken on.
/// @return The message body (no `error: ` prefix, no trailing newline).
export auto render_duplicate_seq(std::int64_t seq, std::string_view run_uid) -> std::string;

/// @brief Render `bench show --json`.
///
/// Field order is fixed and load-bearing (the parity comparison is
/// byte-for-byte, not structural): id, run_uid, plan_id, arm, base_sha,
/// config_hash, config_json, corpus_repo, status, started_at, ended_at,
/// events, touches.
/// @param run_ The run header.
/// @param events The journal rows, already in seq order.
/// @param touches The touch rows, already in id order.
/// @return The complete stdout payload: the single-line JSON object WITH
/// its trailing newline, exactly as the oracle writes it.
export auto render_bench_show_json(const lifecycle::run& run_, std::span<const lifecycle::event_row> events,
                                   std::span<const lifecycle::touch_row> touches) -> std::string;

/// @brief Render `bench show`'s text form.
///
/// Labels are padded to width 13 (`config_hash: ` is the longest). Both
/// `corpus_repo` and `ended_at` lines are OMITTED entirely when unset — they
/// are not rendered as empty. The two section headers are each preceded by a
/// blank line, and both are emitted even when the section is empty
/// (`events (0):` with nothing after it).
/// @param run_ The run header.
/// @param events The journal rows, already in seq order.
/// @param touches The touch rows, already in id order.
/// @return The text block, WITH a trailing newline on the last row.
export auto render_bench_show_text(const lifecycle::run& run_, std::span<const lifecycle::event_row> events,
                                   std::span<const lifecycle::touch_row> touches) -> std::string;

/// @brief Render `run show --json`.
///
/// Carries only id, run_uid, plan_id, arm, status, started_at, ended_at, and
/// events — no base_sha / config_hash / config_json / corpus_repo / touches.
/// @param run_ The run header.
/// @param events The journal rows, already in seq order.
/// @return The complete stdout payload: the single-line JSON object WITH
/// its trailing newline, exactly as the oracle writes it.
export auto render_run_show_json(const lifecycle::run& run_, std::span<const lifecycle::event_row> events) -> std::string;

/// @brief Render `run show`'s text form. Labels padded to width 12.
/// @param run_ The run header.
/// @param events The journal rows, already in seq order.
/// @return The text block, WITH a trailing newline on the last row.
export auto render_run_show_text(const lifecycle::run& run_, std::span<const lifecycle::event_row> events) -> std::string;

/// @brief Render `bench start`'s stdout: the run_uid alone.
///
/// NOT JSON, and there is no `--json` flag on this leaf at all.
/// @param run_uid The minted uid.
/// @return The complete stdout payload: the uid WITH its trailing newline.
export auto render_bench_start(std::string_view run_uid) -> std::string;

/// @brief Render the bare `ok` acknowledgement `bench event` / `bench touch`
/// / `bench finish` print on success.
///
/// All three leaves print exactly this and none of them accepts `--json`.
/// @return The complete stdout payload: the literal `ok` plus a newline.
export auto render_bench_ok() -> std::string;

/// @brief Render `run start`'s output.
///
/// Emitted unconditionally as JSON — the `--json` flag exists on the leaf but
/// changes nothing (oracle-captured: `run start --plan 1` with no `--json`
/// prints the same object).
/// @param run_uid The minted uid.
/// @param plan_id The owning plan.
/// @param arm The arm (`"op"`, or the `--workflow` value).
/// @return The complete stdout payload: the single-line JSON object WITH
/// its trailing newline, exactly as the oracle writes it.
export auto render_run_start_json(std::string_view run_uid, std::int64_t plan_id, std::string_view arm) -> std::string;

/// @brief Render `run event`'s output. Also emitted unconditionally.
/// @param run_uid The run's uid, echoed from the argument (not re-read).
/// @param seq The auto-incremented seq that was used.
/// @param kind The event kind.
/// @return The complete stdout payload: the single-line JSON object WITH
/// its trailing newline, exactly as the oracle writes it.
export auto render_run_event_json(std::string_view run_uid, std::int64_t seq, std::string_view kind) -> std::string;

/// @brief Render `run finish`'s output. Also emitted unconditionally.
/// @param run_uid The run's uid, echoed from the argument.
/// @param status The terminal status that was set.
/// @return The complete stdout payload: the single-line JSON object WITH
/// its trailing newline, exactly as the oracle writes it.
export auto render_run_finish_json(std::string_view run_uid, std::string_view status) -> std::string;

} // namespace planar::engine::runs::render

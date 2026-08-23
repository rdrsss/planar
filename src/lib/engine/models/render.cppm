/// @file render.cppm
/// @brief `planar.engine.models.render` — the byte-exact JSON and text
/// renderers for the thirteen ported `planar models *` schema leaves (plan
/// 996, task 6096).
///
/// Behavior-preserving port (D2) of the rendering half of
/// zig/src/cmd/planar/handlers/models.zig. Every shape below was captured by
/// RUNNING `./zig/zig-out/bin/planar` against a scratch database
/// (`PLANAR_DB` / `PLANAR_CONFIG_PATH` / `HOME` all redirected under /tmp),
/// never read off `--help` and never inferred from a struct definition.
///
/// ## Field order is part of the contract
///
/// Zig's `std.json.Stringify.value` emits struct fields in DECLARATION
/// order, so the wire order is fixed by the Zig struct, not alphabetical and
/// not arbitrary. Every emitter here writes fields in that exact captured
/// order. A reordering would still be valid JSON and would still parse — and
/// would still be a parity break, because parity compares bytes.
///
/// ## Number formatting
///
/// `std::format`'s default floating-point presentation is the shortest
/// round-trippable form, which is what `std.json.Stringify` produces too:
/// `0.8` stays `0.8`, an exact `1.0` prints as `1` (NOT `1.0`), an exact
/// `0.0` prints as `0`, and `0.5839825677481064` keeps all seventeen
/// significant digits. Verified against the oracle's own eval output rather
/// than assumed. The two spellings can still diverge for magnitudes large
/// enough to reach exponent notation (`1e20` vs C++'s `1e+20`); no field
/// rendered here can reach that range — rates are bounded in [0,1] and the
/// latency/cost means come from non-negative integer columns — so the
/// divergence is unreachable rather than merely unlikely.
///
/// ## Escaping
///
/// `append_json_string` matches zig's `std.json.Stringify.encodeJsonString`
/// with default options: the two mandatory escapes, the five short forms,
/// LOWERCASE `\u00xx` for the remaining C0 control bytes, and deliberately NO
/// escaping of `/` or of non-ASCII bytes. This matters here specifically
/// because candidate identifiers, vendor strings, roles, and host ids are all
/// OPAQUE operator-supplied data that the registry stores verbatim.

module;

export module planar.engine.models.render;

import std;
import planar.engine.models.registry;
import planar.engine.models.ranking;
import planar.engine.models.views;

namespace planar::engine::models::render {

/// @brief The one-window legacy-compatibility warning both registry views
/// carry.
///
/// Emitted as a JSON field by `models registry list --json` and
/// `models registry export`, and ALSO as a plain `warning: ` line on STDERR
/// by `models registry export` when `--json` is absent — see
/// `registry_export_stderr_warning`.
export inline constexpr std::string_view migration_warning = "legacy catalog compatibility is one-window and non-authoritative";

/// @brief Append `text` to `out` as a quoted, escaped JSON string.
///
/// Exposed so tests can pin the escaping table directly rather than only
/// through a whole envelope.
/// @param out The buffer to append to.
/// @param text The raw bytes to quote.
export auto append_json_string(std::string& out, std::string_view text) -> void;

/// @brief Render `models registry list --json` / `models registry export`.
///
/// Both leaves emit the SAME envelope — captured byte-for-byte identical:
/// `{"registry_version":1,"candidates":[...],"migration_warning":"..."}`
/// followed by a newline. `export` differs ONLY in the stderr warning it adds
/// when `--json` is absent.
/// @param candidates The registry contents, already in fallback order.
/// @return The complete stdout payload, newline-terminated.
export auto registry_json(std::span<const registry::candidate> candidates) -> std::string;

/// @brief The stderr line `models registry export` writes when `--json` is
/// absent.
///
/// A separate accessor rather than folded into `registry_json` because it
/// goes to a DIFFERENT stream: the JSON stays clean on stdout so a pipeline
/// consuming `models registry export` without `--json` still parses.
/// @return The warning line, newline-terminated.
export auto registry_export_stderr_warning() -> std::string;

/// @brief Render `models registry list` (no `--json`).
///
/// One space-separated line per candidate:
/// `<id> <vendor> <candidate_id> enabled=<bool> order=<n> bindings=<n>
/// observation=<none|present>`. Captured:
///
///     1 anthropic claude-opus-5 enabled=true order=1 bindings=1 observation=present
///
/// `observation` is deliberately a PRESENCE word, not a timestamp: this line
/// answers "is there evidence at all", and the evidence itself lives in the
/// JSON view.
/// @param candidates The registry contents, already in fallback order.
/// @return The complete stdout payload; empty for an empty registry.
export auto registry_text(std::span<const registry::candidate> candidates) -> std::string;

/// @brief Render `models registry add` / `models registry observe`.
///
/// Both print the new row's id and nothing else. `bind`, `unbind`, `update`,
/// and `remove` print NOTHING at all on success (oracle-captured: exit 0,
/// empty stdout) — there is no renderer for them, deliberately.
/// @param id The newly written row id.
/// @return The id, newline-terminated.
export auto id_line(std::int64_t id) -> std::string;

/// @brief Render `models registry eligibility`.
///
/// This leaf has NO `--json` flag — it emits JSON unconditionally (passing
/// `--json` is an exit-2 `error: unknown flag (got --json)`, captured). The
/// shape is:
/// `{"candidate":N,"host":"...","eligible":B,"gates":{six booleans},
///   "reasons":[...]}`
/// @param candidate_id The candidate that was evaluated.
/// @param host_id The host the evaluation was scoped to.
/// @param gates The six evaluated gates.
/// @return The complete stdout payload, newline-terminated.
export auto eligibility_json(std::int64_t candidate_id, std::string_view host_id, const registry::eligibility& gates)
    -> std::string;

/// @brief Render `models registry verify-identity`.
///
/// `{"candidate":N,"identity":"<outcome>"}`, newline-terminated. Also
/// unconditionally JSON.
/// @param candidate_id The candidate that was verified.
/// @param outcome The verification outcome.
/// @return The complete stdout payload, newline-terminated.
export auto verify_identity_json(std::int64_t candidate_id, registry::identity_verification outcome) -> std::string;

/// @brief Render `models evals <cohort flags> --json`.
///
/// `{"version":"routing-ranking-v1","evidence":"declared_experiment",
///   "gates":{...},"rows":[...],"recommended":<string|null>,
///   "no_recommendation_reason":<string|null>}`.
///
/// `recommended` carries the candidate's opaque STRING, not its index — a
/// detail that is easy to get wrong from the Zig source, where the in-memory
/// field is an index that the emitter dereferences.
/// @param outcome The ranking result.
/// @param gate_config The gates the ranking ran under; echoed back so a
/// captured result is self-describing.
/// @return The complete stdout payload, newline-terminated.
export auto evals_json(const ranking::result& outcome, const ranking::gates& gate_config) -> std::string;

/// @brief Render `models evals <cohort flags>` (no `--json`).
///
/// The fixed-width scorecard table, its gate banner, and the recommendation
/// footer. When there are no rows at all the table and footer are BOTH
/// omitted and only `  (no cohort-eligible declared-experiment samples)` is
/// printed — oracle-confirmed, and easy to get wrong by printing an empty
/// table plus a "no recommendation" footer.
/// @param outcome The ranking result.
/// @param gate_config The gates the ranking ran under.
/// @return The complete stdout payload.
export auto evals_text(const ranking::result& outcome, const ranking::gates& gate_config) -> std::string;

/// @brief Render `models experiments --json`.
/// @param experiments The declared experiments, oldest first.
/// @return `{"views_version":"...","experiments":[...]}`, newline-terminated.
export auto experiments_json(std::span<const views::experiment> experiments) -> std::string;

/// @brief Render `models experiments` (no `--json`).
///
/// Emits `no declared routing experiments\n` for an empty set; otherwise a
/// four-line stanza per experiment, each stanza followed by a BLANK line
/// (including the last one — captured).
/// @param experiments The declared experiments, oldest first.
/// @return The complete stdout payload.
export auto experiments_text(std::span<const views::experiment> experiments) -> std::string;

/// @brief Render `models outcomes --json`.
/// @param outcomes The terminal samples, newest first.
/// @return `{"views_version":"...","outcomes":[...]}`, newline-terminated.
export auto outcomes_json(std::span<const views::outcome> outcomes) -> std::string;

/// @brief Render `models outcomes` (no `--json`).
///
/// Emits `no recorded terminal outcomes\n` for an empty set; otherwise a
/// two-line stanza per outcome. The second line is either
/// `counts toward recommendation (quality_success=yes|no)` or
/// `EXCLUDED from recommendations: <reason>` — an exclusion is NEVER printed
/// without its reason, because an unexplained exclusion is indistinguishable
/// from a bug.
/// @param outcomes The terminal samples, newest first.
/// @return The complete stdout payload.
export auto outcomes_text(std::span<const views::outcome> outcomes) -> std::string;

} // namespace planar::engine::models::render

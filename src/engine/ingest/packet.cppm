/// @file packet.cppm
/// @brief `planar.engine.ingest.packet` — the task packet behind `planar task
/// packet <id>`: a task's coordination readiness, its citations as references,
/// and a digest over both.
///
/// The packet compiles current Planar rows and links into a typed input,
/// orders unordered evidence, decides readiness and hashes only semantic
/// fields. Entry points: `assemble_task` (database to packet), `compile_task`
/// (input to packet), `expand_reference` and the `render_*` functions. Expected
/// failures are the `packet_error` and `expand_error` enums; an unready task is
/// a packet with reasons, never an error.
///
/// ## WHY THIS LIVES IN `engine/ingest` AND NOT AN `engine/routing` BUCKET
///
/// A layer-2 `engine_*` may not depend on another layer-2 `engine_*`, and the
/// packet shares the `materialize` module's digest and Markdown-section
/// helpers: `source_digest` and `sha256_hex` digest a reference, and
/// `artifact_section` resolves a `#heading` fragment for `--expand`. A
/// separate bucket would FATAL at configure time.
///
/// ## REFERENCES, NOT FACTS
///
/// The packet carries no `routing_task_facts` row and no section text. A
/// citation is a REFERENCE derived live from the task's `cites` entity links:
/// the cited artifact's id, a locator, a digest and a one-line label. The
/// text of a cited section is fetched on demand with `expand_reference`
/// (`planar task packet <id> --expand <ref>`). Nothing reads
/// `routing_task_facts` on master any more; the table is dropped by a later
/// migration.
///
/// ## READINESS COVERS COORDINATION FACTS ONLY
///
/// `ready` answers one question: may an agent take this task now? It looks at
/// the task's own row, its plan, its citations, and what holds it back from
/// another entity. It never looks at the spec documents, the task's body, test
/// scenarios, touches or validation sections; a task created with `task add`
/// and nothing else is ready.
///
/// ## THE REASON LIST IS CONTRACT, AND ITS ORDER IS DOCUMENTED
///
/// `task packet <id> --json` carries `ready` and `reasons`; `ready: false` is
/// a documented STOP. A reader selects a reason BY NAME and never by position.
/// The order is fixed so that two renderings of the same packet are
/// byte-identical, and it is this, `compile_task` emitting each reason once
/// and the first occurrence winning:
///
///   1. `missing_title`            — the title is empty or blank.
///   2. `invalid_task_status`      — the task is not claimable. A task is
///      claimable when it is `todo`, or `doing` with no live claim (its claim
///      is stale, expired or absent: recovery). `done`, `cancelled`, `blocked`
///      and a `doing` task under a LIVE claim report this reason. `task
///      packet` takes no caller, so a live claim always makes the task not
///      ready, whoever asks.
///   3. `missing_owning_plan`      — the task has no plan.
///   4. `unresolved_citation`      — a cited artifact has no digest or is not
///      `active`, `draft` or `ready`.
///   5. `blocked_by_open_task`     — a `depends-on` link from the task reaches
///      a task that is not `done` or `cancelled`.
///   6. `open_question_linked`     — a question linked to the task is `open`.
///   7. `proposed_decision_linked` — a decision linked to the task is
///      `proposed`.
///
/// Reasons 5 to 7 are decided by `planar.coordination`, the same functions
/// `recommend-strategy` rules 5 and 6 call, so the packet and the rule cannot
/// disagree about a link. `plan next` and `recommend-strategy` rule 7 read
/// `ready()` from this packet.
///
/// Packets only compare within one `policy_version`, which is why it rides in
/// the JSON ENVELOPE and not only inside the canonical body: a consumer has to
/// know two packets were built under the same rules BEFORE comparing their
/// digests, and digging the version out of the canonical body would mean
/// parsing the very thing whose format the version describes.
///
/// **A CLAIM ROW IS ONLY A BLOCK WHILE ITS LEASE IS LIVE.** `claim_evidence`
/// selects `status = 'active'` rows and re-derives the status as `active` or
/// `expired` from `lease_expires_at` against now. An `active` row blocks a
/// `doing` task. An `expired` row, and a claim already marked `stale`, do not:
/// the task can be reclaimed.
///
/// **THE DIGEST IS COMPUTED OVER THE SEMANTIC CANONICAL FORM, THE `canonical`
/// FIELD OVER THE DISPLAY FORM.** They differ by exactly the `display_label`
/// key. Renaming a plan therefore changes the `canonical` bytes and leaves the
/// digest alone, which is what lets the digest certify "same packet" across a
/// cosmetic edit.
///
/// READ-ONLY: this module only SELECTs. No writes, no migration.
module;

export module planar.engine.ingest.packet;

import std;
import planar.db;

namespace planar::engine::ingest::packet {

/// @brief The rule-set version every packet is stamped with.
///
/// Packets are comparable only within one version. It rides in the JSON
/// envelope as `policy_version` and inside the canonical body as `policy`.
export inline constexpr std::string_view policy_version = "routing-packet-v3";

/// @brief One piece of evidence the packet compiled, with its provenance and
/// freshness.
///
/// A bare `evidence` is required, covered, `ready` and `current`, so a loader
/// that sets only the identity fields produces a well-formed row.
export struct evidence {
  std::string  kind;                         ///< Entity or fact kind, e.g. `product_spec`, `claim`.
  std::int64_t id = 0;                       ///< Source row id.
  std::string  locator;                      ///< `artifact:12#Overview`, `next_action`, a path, …
  std::string  text;                         ///< The semantic payload the digest is taken over.
  std::string  display_label;                ///< Human label; in `canonical`, never in the digest.
  std::string  source_digest;                ///< Digest recorded when the evidence was materialized.
  std::string  current_digest;               ///< Digest recomputed live; empty when unresolvable.
  bool         required   = true;            ///< Whether readiness depends on this row.
  bool         covered    = true;            ///< Scenario/coverage membership.
  std::string  status     = "ready";         ///< Lifecycle status, checked per evidence class.
  std::string  provenance = "planar";        ///< Where the row came from, e.g. `artifact:12`.
  std::string  materializer_version;         ///< Version stored with the fact.
  std::string  current_materializer_version; ///< Version the running binary would write.
  std::string  freshness = "current";        ///< `current` or `stale`.
};

/// @brief One citation of an artifact, derived from a `cites` entity link.
///
/// A reference names a cited artifact and carries no text from it; the text
/// comes from `expand_reference`. Defaults mirror `evidence`.
export struct reference {
  std::string  ref;               ///< `artifact:<id>`, the argument `--expand` takes.
  std::string  kind;              ///< The cited artifact's kind, e.g. `tech_spec`.
  std::int64_t id      = 0;       ///< The cited artifact's id.
  std::string  locator = "body";  ///< `body` for the whole artifact; the entity link carries no finer locator.
  std::string  digest;            ///< `source_digest("artifact", id, locator, body)`, 64 lowercase hex chars.
  std::string  label;             ///< One-line label: the artifact's title. In `canonical`, never in the digest.
  std::string  status = "active"; ///< The artifact's lifecycle status.
};

/// @brief Everything the compiler reads about one task.
///
/// Field order is load-bearing: it is the key order of both the JSON `input`
/// object and the canonical body.
export struct task_input {
  std::int64_t           task_id = 0;        ///< `tasks.id`.
  std::string            status;             ///< `tasks.status`.
  std::string            title;              ///< `tasks.title`.
  std::string            body;               ///< `tasks.body`, coalesced to empty.
  std::string            next_action;        ///< `tasks.next_action`, coalesced to empty.
  std::vector<evidence>  owning_plans;       ///< The plan the task belongs to; 0 or 1 row.
  std::vector<reference> references;         ///< The artifacts the task `cites`, ordered by kind then id.
  std::vector<evidence>  dependencies;       ///< Tasks this one `depends-on`, with their `tasks.status`.
  std::vector<evidence>  open_questions;     ///< Linked questions whose status is `open`.
  std::vector<evidence>  proposed_decisions; ///< Linked decisions whose status is `proposed`.
  std::vector<evidence>  claims;             ///< Active claims; a lapsed lease is reported as `expired`.
};

/// @brief Why a packet is not ready.
///
/// Enumerator order is declaration order and is NOT the emission order; see
/// this file's header for the emission order, which `compile_task` fixes.
export enum class readiness_reason : std::uint8_t {
  missing_title,
  invalid_task_status,
  missing_owning_plan,
  unresolved_citation,
  blocked_by_open_task,
  open_question_linked,
  proposed_decision_linked,
};

/// @brief The wire spelling of a reason: its enumerator name.
/// @param reason The reason.
/// @return Its snake_case name, e.g. `"blocked_by_open_task"`.
export [[nodiscard]] auto reason_name(readiness_reason reason) -> std::string_view;

/// @brief A compiled packet: the input it was built from, both canonical
/// renderings' outcome, and the readiness verdict.
export struct task_packet {
  task_input                    input;     ///< The evidence the packet was compiled from.
  std::string                   canonical; ///< Canonical body INCLUDING `display_label`.
  std::string                   digest;    ///< 64 lowercase hex chars over the SEMANTIC body.
  std::vector<readiness_reason> reasons;   ///< In the documented order, each at most once.

  /// @brief Whether the task is dispatchable.
  /// @return True iff there are no reasons.
  [[nodiscard]] auto ready() const -> bool {
    return reasons.empty();
  }
};

/// @brief Why assembly failed.
export enum class packet_error : std::uint8_t {
  task_not_found, ///< No `tasks` row with that id (`assemble_task`).
  query_failed,   ///< SQL failure.
};

/// @brief Compile a packet from a caller-supplied input.
///
/// Pure: no database, no clock. Exported so the readiness rules can be tested
/// on inputs a fixture cannot reach through SQL.
/// @param input The evidence to compile.
/// @return The packet, always — an unready input is a packet with reasons,
/// never an error.
export [[nodiscard]] auto compile_task(const task_input& input) -> task_packet;

/// @brief Reassemble the packet from the current database snapshot.
/// @param conn An open database connection.
/// @param task_id The task to compile.
/// @return The packet, or `task_not_found` / `query_failed`.
export [[nodiscard]] auto assemble_task(db::connection& conn, std::int64_t task_id) -> std::expected<task_packet, packet_error>;

/// @brief Why `expand_reference` failed.
export enum class expand_error : std::uint8_t {
  task_not_found,    ///< No `tasks` row with that id.
  unknown_reference, ///< The reference is malformed or is not among the task's `cites` links.
  section_not_found, ///< The artifact is cited but has no section with the requested heading.
  query_failed,      ///< SQL failure.
};

/// @brief The text a reference resolves to.
export struct expansion {
  std::string ref;    ///< The reference as the caller wrote it.
  std::string digest; ///< `source_digest("artifact", id, locator, text)`; equals the reference's digest for a whole artifact.
  std::string text;   ///< The whole artifact body, or the section under the requested heading.
};

/// @brief Resolve one of the task's references to its text.
///
/// `ref` is `artifact:<id>` for the whole body, or `artifact:<id>#<heading>`
/// for the section under that heading (matched case-insensitively, nested
/// subsections included). The artifact must be one of the task's `cites`
/// links; anything else is `unknown_reference`, so a caller cannot read an
/// arbitrary artifact through a task it does not cite.
/// @param conn An open database connection.
/// @param task_id The task whose references are searched.
/// @param ref The reference to resolve.
/// @return The expansion, or the failure.
export [[nodiscard]] auto expand_reference(db::connection& conn, std::int64_t task_id, std::string_view ref)
    -> std::expected<expansion, expand_error>;

/// @brief Render an expansion as text.
/// @param value A resolved expansion.
/// @return The text, with a trailing newline added when it lacks one.
export [[nodiscard]] auto render_expansion_text(const expansion& value) -> std::string;

/// @brief Render an expansion as `{"ref":...,"digest":...,"text":...}`.
/// @param value A resolved expansion.
/// @return A COMPLETE stdout payload including its trailing newline.
export [[nodiscard]] auto render_expansion_json(const expansion& value) -> std::string;

/// @brief Render the operator-facing text form.
/// @param packet A compiled packet.
/// @return A COMPLETE stdout payload including its trailing newline.
export [[nodiscard]] auto render_text(const task_packet& packet) -> std::string;

/// @brief Render the machine-readable envelope.
///
/// The envelope carries `policy_version` alongside `ready`, `input`, `digest`
/// and `reasons`. It carries no `canonical` body: `input` is the only
/// serialized copy of the evidence, so the packet is not emitted twice.
/// `input.references` lists `{ref, kind, id, locator, digest, label, status}`
/// per cited artifact and carries no text.
/// @param packet A compiled packet.
/// @return A COMPLETE stdout payload including its trailing newline.
export [[nodiscard]] auto render_json(const task_packet& packet) -> std::string;

} // namespace planar::engine::ingest::packet

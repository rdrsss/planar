/// @file sync.cppm
/// @brief `planar.engine.external.sync` — link-level pull/push/resolve
/// orchestration and the FIELD-LEVEL conflict engine (plan 996, task 6041).
///
/// Behavior-preserving port (D2) of all of
/// `zig/src/engine/external/sync.zig`. Nothing from that file is deferred.
///
/// ## The conflict rule, derived by RUNNING the oracle
///
/// The rules below were not read off the resolver. They were derived by
/// seeding each divergence against a loopback Jira fixture and running
/// `./zig/zig-out/bin/planar sync pull` on a scratch database; the captures
/// are reproduced in sync.t.cpp. What that produced:
///
/// A field CONFLICTS when ALL THREE hold, evaluated per field independently:
///
///   1. the REMOTE differs from the stored baseline, AND
///   2. the LOCAL differs from the stored baseline, AND
///   3. remote and local differ from EACH OTHER.
///
/// Only `title` and `status` participate — no other field is compared, and
/// `status` additionally requires a NON-EMPTY remote status (an unmappable
/// provider status reads as "the remote has no opinion", not as a change).
///
/// Four consequences that a from-source reading gets wrong more often than
/// not, each one captured from a real run:
///
///   - **Conflict is per-field but abort is whole-link.** With title in
///     conflict and status changed only remotely, the oracle reported
///     `fields_changed:["title"]` — and applied NEITHER field. The task row
///     and the baseline were both untouched.
///   - **Local-only change is `noop`, and the BASELINE DOES NOT MOVE.** When
///     the remote is unchanged since the baseline, `allow_title` /
///     `allow_status` go false, nothing is applied, and the baseline keeps
///     its OLD value rather than absorbing the local edit. That is what
///     makes the NEXT remote change a detectable conflict instead of a
///     silent overwrite.
///   - **Conflict detection runs for `two-way` links ONLY.** A `read-only`
///     link applies the remote unconditionally — captured: a read-only pull
///     over a locally-edited task overwrote it, `fields_changed:
///     ["title","status"]`, no conflict. A `write-back` link pulls as `noop`
///     and touches nothing.
///   - **No baseline means no conflict is possible.** The first pull on a
///     fresh link just applies and records a baseline.
///
/// ## Adapter failure is an OUTCOME, not an error
///
/// An adapter failure is `outcome::error` with the Zig error tag in `detail`
/// — a RESULT, not a `sync_error`. `pull_link` and `push_link` write that
/// best-effort row (link status `error`, one `<direction>/error` event)
/// OUTSIDE the transaction and return normally. Captured from the oracle: an
/// unreachable port produced `{"outcome":"error","detail":"TransportFailed"}`
/// and process exit code 0. Only a CONFLICT drives the CLI's exit 3.
///
/// ## `resolve_conflict` is a compare-and-swap with FOUR guards
///
/// Every one of them was observed refusing a real attempt without mutating
/// anything (task row, link row, `sync_events`, and the fixture's PUT count
/// all unchanged):
///
/// IN THE ORDER THE CODE RUNS THEM, which is not the order an earlier edition
/// of this comment gave (task 6297 — see below):
///
///   1. the named event must BE a conflict — otherwise `not_conflict`;
///   2. the supplied token must equal the stored evidence token — otherwise
///      `evidence_changed`;
///   3. the named event must still be the LATEST event on its link, and the
///      link must still be in `conflict` — otherwise `stale_conflict`;
///   4. the supplied local `updated_at` must equal the entity's CURRENT one;
///   5. a FRESH adapter pull must still match the recorded evidence in title,
///      status AND version — and BOTH the recorded and the fresh version must
///      be NON-EMPTY. A provider that exposes no version can never authorize
///      a resolution, however well its field values match.
///
/// THE ORDER IS NOT DECORATIVE, and this comment having it backwards is why
/// an oracle defect survived the original port of this module. The earlier
/// edition listed stale-then-token; the code runs token before stale, and ran
/// the CAS guard ahead of the is-this-a-conflict check entirely (task 6296),
/// so pointing the verb at a NON-conflict event reported "evidence or
/// approved local version changed" — telling the operator their data had
/// raced when in fact they had named the wrong event. A reader checking
/// whether the guard order was sensible read this header, found a sensible
/// order, and stopped. 6296 is fixed; the header is corrected here so the
/// next such reader is checking against the code rather than against prose.
///
/// The inline labels in `resolve_conflict` still read `Guard 2` before
/// `Guard 1a`, preserving the numbering this list used to carry. They are
/// left alone deliberately: renumbering them would silently rewrite the
/// history of which guard came from where, and the sequence above is now the
/// authority.
///
/// `keep=local` pushes to the provider and leaves the entity alone; the event
/// it writes has direction `push`. `keep=remote` writes the remote onto the
/// entity and sends NOTHING — captured: zero PUTs — and its event has
/// direction `pull`. Both write detail
/// `resolved=<keep>; from sync_event=<id>` and set the baseline to the
/// resolved local values.
///
/// ## The adapter arrives as an interface, which is what keeps layering legal
///
/// The Zig original takes `adapter: anytype`. Here it is
/// `planar.adapter`'s `external_adapter&` — a LAYER-1 interface — so this
/// layer-2 bucket never names `engine_extsync`, which would be the
/// same-layer edge D18 forbids. Every test in sync.t.cpp drives a stub
/// adapter with no HTTP at all.
module;

export module planar.engine.external.sync;

import std;
import planar.db;
import planar.adapter;
import planar.engine.external.link;

namespace planar::engine::external::sync {

/// @brief What one pull or push did. Mirrors the Zig `Outcome`.
export enum class outcome : std::uint8_t {
  ok,       ///< Fields changed.
  conflict, ///< Both sides changed the same field differently.
  noop,     ///< Nothing to do.
  error,    ///< The adapter failed.
};

/// @brief The `sync_events.outcome` text for an outcome.
/// @param value The outcome.
/// @return The column value.
export auto outcome_to_event_text(outcome value) -> std::string_view;

/// @brief Which side a resolution keeps.
export enum class resolve_keep : std::uint8_t {
  local,  ///< Push the local entity to the provider.
  remote, ///< Overwrite the local entity from the provider.
};

/// @brief The text `sync_events.detail` records for a keep choice.
/// @param keep The choice.
/// @return `local` or `remote`.
export auto resolve_keep_to_text(resolve_keep keep) -> std::string_view;

/// @brief What one pull did.
///
/// ## `remote_title`/`remote_status`: EMITTED, not applied (decision 996)
///
/// Plan 996, task 6419 removed this module's `apply_remote_to_local`: a
/// `planar-ext` connection is authorizer-restricted to `external_links` /
/// `external_systems` / `sync_events` (decision 995), so a write into
/// `tasks`/`plans`/`questions`/`artifacts` would refuse at `prepare()` even
/// if this module still attempted one. `pull_link` now only DETECTS which
/// fields differ and reports the remote's values here — it never writes
/// them. `result == ok` means "the remote differs from the local entity in
/// `fields_changed`, and here is what the remote holds"; the write itself
/// is the caller's job, through `planar`, informed by these two fields. Set
/// only when the corresponding name appears in `fields_changed`.
export struct pull_result {
  std::int64_t               link_id = 0;             ///< The link.
  outcome                    result  = outcome::noop; ///< What happened.
  std::vector<std::string>   fields_changed;          ///< Which fields differ, or (on conflict) which CONFLICTED.
  std::string                detail;                  ///< Free text; the adapter error tag on `error`.
  std::optional<std::string> remote_title;            ///< The remote's title, when `title` is in `fields_changed`.
  std::optional<std::string> remote_status;           ///< The remote's status, when `status` is in `fields_changed`.
};

/// @brief What one push did. Same shape as `pull_result`; kept as a distinct
/// type because the Zig original does, and because the two grow apart at the
/// CLI boundary.
export struct push_result {
  std::int64_t             link_id = 0;             ///< The link.
  outcome                  result  = outcome::noop; ///< What happened.
  std::vector<std::string> fields_changed;          ///< Which fields the adapter reported applying.
  std::string              detail;                  ///< Free text; the adapter error tag on `error`.
};

/// @brief What one resolution did.
export struct resolve_result {
  bool         ok           = false;               ///< Always true on success; part of the JSON shape.
  std::int64_t event_id     = 0;                   ///< The conflict event that was resolved.
  std::int64_t new_event_id = 0;                   ///< The resolution event that was written.
  resolve_keep keep         = resolve_keep::local; ///< Which side was kept.
};

/// @brief One row of `sync status`.
export struct status_row {
  std::int64_t               link_id     = 0;                                ///< The link.
  link::external_entity_kind entity_kind = link::external_entity_kind::plan; ///< Which local table.
  std::int64_t               entity_id   = 0;                                ///< The local row.
  std::string                external_id;                                    ///< The provider's id.
  std::int64_t               system_id = 0;                                  ///< The registered system.
  std::optional<std::string> last_synced_at;                                 ///< When last attempted.
  link::sync_status          last_sync_status = link::sync_status::never;    ///< The last outcome.
};

/// @brief One `sync_events` row, as `audit trail --link` surfaces it.
export struct sync_event {
  std::int64_t               id = 0;         ///< The event id.
  std::string                direction;      ///< `pull` or `push`.
  std::string                event_outcome;  ///< `ok`, `conflict`, `noop` or `error`.
  std::optional<std::string> fields_changed; ///< A JSON array of field names, or NULL.
  std::optional<std::string> detail;         ///< Free text, or NULL.
  std::optional<std::string> context_json;   ///< The conflict-evidence blob, or NULL.
  std::string                at;             ///< When it happened.
};

/// @brief Why a sync operation failed.
///
/// Mirrors the Zig `Error` set minus `LinkExists`, which no ported path can
/// produce (nothing here inserts an `external_links` row).
export enum class sync_error : std::uint8_t {
  not_found,               ///< The link, event or local entity does not exist.
  query_failed,            ///< SQL failure.
  read_only,               ///< A push was attempted on a `read-only` link.
  unsupported_entity_kind, ///< The link points at a table with no title/status/updated_at triple.
  not_conflict,            ///< The named event is not a conflict event.
  stale_conflict,          ///< The named event is no longer the link's latest, or the link left `conflict`.
  evidence_changed,        ///< A CAS guard rejected the supplied or freshly-read evidence.
  adapter_failed,          ///< The adapter failed during a resolution.
};

/// @brief Every `sync_events` row for one link, OLDEST FIRST.
///
/// Order is load-bearing: `audit trail` renders them in this order and every
/// caller that wants "the current conflict" takes the LAST element.
/// @param conn An open, migrated database connection.
/// @param link_id The link.
/// @return The events, or the failure.
export auto events_for_link(db::connection& conn, std::int64_t link_id) -> std::expected<std::vector<sync_event>, sync_error>;

/// @brief Pull one link: read the remote, detect conflicts, and EMIT what
/// differs — it does not write the local entity (decision 996, task 6419).
///
/// See this module's header for the full derived rule set and
/// `pull_result`'s header for what `remote_title`/`remote_status` carry.
/// Conflict detection is unchanged: it still reads the baseline and the
/// local entity to decide `ok` vs. `conflict` vs. `noop`. What changed is
/// the non-conflict arm's WRITE TARGET, not its baseline formula: it used
/// to call `apply_remote_to_local` (writing `tasks`/`plans`/…) and then
/// re-read the entity to store a fresh baseline in `external_links`; it now
/// skips the entity write and computes the SAME would-be baseline directly
/// (see `local_diff`'s `diff_result`), storing it in `external_links` —
/// the one table this write was always in, and the one decision 995 keeps
/// allowing. The baseline still advances on every non-conflict pull,
/// including a first noop pull with nothing to report, exactly as before;
/// only `tasks`/`plans`/`questions`/`artifacts` stopped being written.
/// Writes exactly one `sync_events` row and always updates the link's sync
/// state.
/// @param conn An open, migrated database connection.
/// @param row The link to pull.
/// @param provider The adapter to read through.
/// @return What happened, or the failure.
export auto pull_link(db::connection& conn, const link::ext_link& row, const adapter::external_adapter& provider)
    -> std::expected<pull_result, sync_error>;

/// @brief Push one link: send the local title and status to the provider and
/// record the attempt.
///
/// Does NOT read the remote first and does NOT detect conflicts — a push is
/// unconditional by design. A `read-only` link is refused with
/// `sync_error::read_only` before anything is sent.
/// @param conn An open, migrated database connection.
/// @param row The link to push.
/// @param provider The adapter to write through.
/// @return What happened, or the failure.
export auto push_link(db::connection& conn, const link::ext_link& row, const adapter::external_adapter& provider)
    -> std::expected<push_result, sync_error>;

/// @brief Resolve one conflict event, keeping one side, under four CAS
/// guards.
///
/// See this module's header for the guards and what each side does. Every
/// guard failure leaves the database and the provider untouched.
/// @param conn An open, migrated database connection.
/// @param event_id The conflict event to resolve.
/// @param keep Which side wins.
/// @param expected_evidence_token The token the operator read out of the
/// event's evidence blob.
/// @param expected_local_updated_at The entity `updated_at` the operator
/// approved against.
/// @param provider The adapter, used to re-read the remote and (for
/// `keep=local`) to push.
/// @return What happened, or the failure.
export auto resolve_conflict(db::connection& conn, std::int64_t event_id, resolve_keep keep,
                             std::string_view expected_evidence_token, std::string_view expected_local_updated_at,
                             const adapter::external_adapter& provider) -> std::expected<resolve_result, sync_error>;

/// @brief The `sync status` row set for the links matching `filter`.
/// @param conn An open, migrated database connection.
/// @param filter Which links to report on.
/// @return The rows, or the failure.
export auto status(db::connection& conn, const link::list_filter& filter) -> std::expected<std::vector<status_row>, sync_error>;

/// @brief The conflict-evidence token for one seeded divergence.
///
/// Exposed because it is the CAS key an operator copies out of
/// `audit trail --link --json` and back into `sync resolve
/// --evidence-token`, so it is a contract rather than an internal detail —
/// and because pinning it directly is the only way to prove the formula
/// without seeding a whole conflict. The input is
/// `v1\0<link_id>\0<local_title>\0<local_status>\0<local_updated_at>
/// \0<remote_title>\0<remote_status>\0<remote_version>`, SHA-256, lowercase
/// hex.
/// @param link_id The link.
/// @param local_title The local title at the time of the conflict.
/// @param local_status The local status.
/// @param local_updated_at The local `updated_at`.
/// @param remote_title The remote title.
/// @param remote_status The remote status, in Planar's vocabulary.
/// @param remote_version The provider's version marker.
/// @return The 64-character lowercase hex token.
export auto evidence_token(std::int64_t link_id, std::string_view local_title, std::string_view local_status,
                           std::string_view local_updated_at, std::string_view remote_title, std::string_view remote_status,
                           std::string_view remote_version) -> std::string;

} // namespace planar::engine::external::sync

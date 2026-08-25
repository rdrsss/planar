/// @file audit.cppm
/// @brief `planar.policy.audit` — the append-only `audit_log` write path
/// (plan 996 / plan 1001, task 6100). Layer 1.
///
/// PROBLEM this module closes. Thirteen ported engine buckets' CMakeLists
/// each documented, independently, that they omit `policy.audit.record`
/// because no such module existed in the C++ tree; `engine_identity`'s
/// `association.cppm` documents the same. The Zig oracle's live database
/// holds tens of thousands of `audit_log` rows and the C++ binaries wrote
/// none. Nothing observable broke, which is exactly the hazard: the
/// `audit` verb family (`planar audit trail`, session-capture correlation,
/// ext-sync reconciliation) is a later port that READS these rows, and
/// would have read an empty table that the port itself emptied. A missing
/// row is invisible unless a test asserts it exists — no exit code and no
/// stdout byte moves.
///
/// LAYER 1 is forced, not chosen. Every layer-2 `engine_*` bucket needs
/// this, and `cmake/architecture.cmake` FATALs at configure time on a
/// layer-2 -> layer-2 edge (D15/D18). Same shape as `scope_ref` and
/// `json_text` under D19: a shared engine primitive lives one layer down
/// so the same-layer prohibition can stay strict without duplication.
/// DEPENDS db, for the same reason `scope_ref` does — the module IS a
/// DB write primitive.
///
/// ORACLE PROVENANCE. Everything below was derived by RUNNING the Zig
/// binary against scratch databases and reading `audit_log` back, never
/// from reading the Zig source. Session (two scratch DBs, two projects,
/// `PLANAR_DB` + `PLANAR_CONFIG_PATH` isolated):
///
///     verb           kind         summary
///     -------------  -----------  ------------------------------------
///     create         association  create association 'project:p'
///     link           association  add project 'proj' to association 'project:p'
///     unlink         association  remove project 'proj' from association 'project:p'
///     create         plan         create plan 'Plan One'
///     update         plan         <NULL>
///     status_change  plan         recompute plan 1: draft -> active; tasks todo=1 doing=1 blocked=0 done=0 cancelled=0
///     create         task         create task 'Task One'
///     status_change  task         <NULL>        (via `task update --status`)
///     status_change  task         done          (via `task done`)
///     status_change  task         cancelled     (via `task cancel`)
///     create         annotation   create annotation 'A1'
///     update         annotation   <NULL>
///     status_change  annotation   resolve / dismiss / archive
///     delete         annotation   <NULL>
///     create         question     create question 'Q?'
///     status_change  question     answer: A
///     create         session      start session vendor=cli
///
/// Three findings from that session that the source would not have given
/// up, and that the shape of this API is built around:
///
///   1. `actor` and `scope` are **always NULL** on a real CLI invocation.
///      Both columns exist, both are indexed, and nothing on the operator
///      path populates either. They are kept in `record_args` because the
///      oracle's own struct keeps them and a future session-aware caller
///      will want them — but no ported call site passes one, and a test
///      asserting a non-NULL `actor` would be asserting a behaviour the
///      oracle does not have.
///   2. `summary` is NULL for plain `update` and `delete`, and for the
///      `status_change` a generic `task update --status` writes. It is NOT
///      "always populated"; a caller that helpfully synthesised one would
///      diverge invisibly.
///   3. A FAILED mutation writes NO row. Every observed refusal (`task
///      block` on an illegal transition, `task reopen`, `annotate update
///      --status dismissed` from `archived`) left `audit_log` untouched.
///      `record` is therefore called AFTER the mutation's own write
///      succeeds, never before and never speculatively.
///
/// Not audited, also established by running rather than reading:
/// `annotate tag` / `annotate tag --remove` write no row at all, and
/// `planar init`'s project registration writes none either (the
/// association it creates does).
///
/// TRANSACTIONS. The oracle does not wrap the mutation and its audit row
/// in one transaction, and neither does this module — `record` issues a
/// single INSERT on the connection it is handed. A caller that has already
/// opened a transaction gets the row inside it for free.
module;

export module planar.policy.audit;

import std;
import planar.db;

export namespace planar::policy::audit {

/// @brief The `audit_log.verb` domain. Mirrors the CHECK constraint
/// migration 14 installs: any other value is rejected by SQLite itself,
/// which is why this is an enum rather than a string at the API surface.
enum class verb : std::uint8_t {
  create,        ///< Entity was created.
  update,        ///< Entity's fields changed, with no status transition.
  delete_,       ///< Entity was removed. Trailing underscore: `delete` is a keyword.
  status_change, ///< Entity's lifecycle status moved.
  link,          ///< A relationship was established.
  unlink,        ///< A relationship was removed.
};

/// @brief The exact text stored in `audit_log.verb`.
/// @param v The verb.
/// @return The lowercase snake_case spelling the CHECK constraint accepts.
auto verb_to_text(verb v) -> std::string_view;

/// @brief What the row is about. `kind` is a free string rather than an
/// enum ON PURPOSE — the oracle stores it as text so the log survives
/// entity-kind additions without a schema change, and a port that
/// narrowed it to an enum would refuse rows the oracle writes.
struct entity_ref {
  std::string_view kind; ///< `"plan"`, `"task"`, `"annotation"`, ...
  std::int64_t     id;   ///< The entity's primary key.
};

/// @brief One audit-log entry.
struct record_args {
  audit::verb                     verb;    ///< What happened.
  entity_ref                      entity;  ///< What it happened to.
  std::optional<std::string_view> actor;   ///< Always unset on the CLI path today (see file header).
  std::optional<std::string_view> scope;   ///< Always unset on the CLI path today (see file header).
  std::optional<std::string_view> summary; ///< One-line human summary; genuinely NULL for `update`/`delete`.
};

/// @brief Error surface. Mirrors zig `policy.audit.Error`, which has the
/// single member `WriteFailed`.
enum class audit_error : std::uint8_t {
  write_failed, ///< The INSERT did not go through.
};

/// @brief Append one row to `audit_log`.
///
/// Call this AFTER the mutation's own write has succeeded — a refused
/// mutation leaves no audit row in the oracle, and calling ahead of the
/// write would invent one. Every value is bound positionally, so a caller
/// string never participates in SQL parsing.
/// @param conn An open, migrated database connection.
/// @param args The row to write.
/// @return Nothing on success, `write_failed` when the INSERT fails.
auto record(db::connection& conn, const record_args& args) -> std::expected<void, audit_error>;

} // namespace planar::policy::audit

/// @file doctor.cppm
/// @brief `planar.engine.workspace.doctor` — the diagnose-and-repair pass
/// behind `planar workspace doctor` (plan 996, task 6110).
///
/// Behavior-preserving port (D2) of
/// zig/src/cmd/planar/handlers/workspace/doctor.zig.
///
/// ## DOCTOR IS NOT READ-ONLY, and it does not write where you expect
///
/// Two facts, both established by probing rather than by reading the name:
///
///   1. Asking `doctor` to DIAGNOSE also makes it REPAIR. It creates the state
///      directory and reinstalls root guidance symlinks as a side effect of
///      being asked what is wrong. There is no `--dry-run`.
///   2. It writes to the association's recorded `config_json.root_path`, NOT to
///      the current working directory. Run from an unrelated directory, doctor
///      still installed `AGENTS.md` / `CLAUDE.md` into the root recorded at
///      `workspace init` time.
///
/// The second point corrects a natural assumption: isolating the CWD protects
/// nothing here. The destination comes out of the DATABASE, so the protection
/// is a scratch `PLANAR_DB`. Against a real database, doctor writes into the
/// operator's real workspace root from any cwd whatsoever.
///
/// ## It never refuses
///
/// Unlike `routing show` / `routing build` / `regenerate`, doctor takes NO
/// positional and iterates EVERY org. An empty database is `{"orgs":[]}` and
/// exit 0, not the "no org associations registered" refusal its siblings emit;
/// several orgs is several reports, not the ambiguity refusal. Oracle-captured
/// on both.
///
/// ## `issues_found` is a length, not a repair count
///
/// It is literally `issues_repaired.size()`. The two are ALWAYS equal, and the
/// array includes `missing` and `error` entries that were NOT repaired. So a
/// report reading `repaired 2 issues` may have repaired nothing at all — both
/// entries could be `missing` rows. The names are misleading; the behavior is
/// preserved and named here rather than corrected.
///
/// ## The dangling-symlink window
///
/// On a fresh `workspace init` with no `regenerate` yet, one pass reports
/// `AGENTS.md` as `missing` AND installs links pointing at it in the same run —
/// so the links dangle until `regenerate` writes the target. That is the
/// oracle's behavior; the sequence is init -> regenerate -> doctor, and doctor
/// run early leaves a recoverable but broken state.
///
/// ## Early returns lose later diagnostics
///
/// Two failures stop the pass for that org and return what has accumulated: a
/// layout that cannot be computed, and a state directory that cannot be
/// created. A symlink-install failure also returns early. Every other problem
/// is appended and the pass continues.

module;

export module planar.engine.workspace.doctor;

import std;
import planar.db;
import planar.engine.workspace.identity;

namespace planar::engine::workspace::doctor {

/// @brief One diagnosis line.
export struct issue {
  std::string kind;   ///< `fix` / `missing` / `error` — see the report shape below.
  std::string detail; ///< Operator-facing prose; carries absolute paths.
};

/// @brief One org's diagnosis.
export struct org_report {
  std::string        slug;             ///< The org's slug.
  std::int64_t       org_id       = 0; ///< The org's id.
  std::int64_t       issues_found = 0; ///< ALWAYS equals `issues.size()` — see the header.
  std::vector<issue> issues;           ///< Rendered as `issues_repaired` on the wire.
};

/// @brief What the association's `config_json` says about repairing its root.
export enum class workspace_shape {
  meta_repo, ///< `workspace_shape: "meta-repo"`; root guidance is repo-owned, skip it.
  non_meta,  ///< `"sibling"`, or the key absent entirely; install root guidance.
  uncertain, ///< Anything unreadable; skip repair AND report why.
};

/// @brief The shape decision plus, when uncertain, the reason to report.
export struct shape_check {
  doctor::workspace_shape shape = doctor::workspace_shape::non_meta; ///< The decision.
  std::string             reason;                                    ///< Non-empty ONLY for `uncertain`.
};

/// @brief Classify one association's `config_json`.
///
/// Every failure path yields `uncertain` with a DIFFERENT reason, and each one
/// ends with the same `; skipping root guidance repair` clause:
///
///   row absent          `workspace config row missing`
///   column NULL/empty   `workspace config_json is missing`
///   unparseable         `workspace config_json is malformed`
///   not an object       `workspace config_json is not an object`
///   no `root_path`      `workspace config_json has no root_path`
///   bad `root_path`     `workspace config_json has invalid root_path`
///   non-string shape    `workspace config_json has invalid workspace_shape`
///   unknown shape       `workspace config_json has unknown workspace_shape`
///
/// Note `root_path` is validated BEFORE `workspace_shape` is even read, which
/// is why `non_meta` always implies a usable root path.
/// @param conn An open connection.
/// @param org_id The association to classify.
/// @return The decision, or nullopt when the query itself failed.
export auto classify_shape(db::connection& conn, std::int64_t org_id) -> std::optional<shape_check>;

/// @brief Diagnose (and repair) every org association.
///
/// Iterates `list_orgs` in `id` order. Never refuses: zero orgs is an empty
/// vector and no error.
/// @param conn An open connection.
/// @param env Environment lookup callable, for `$PLANAR_HOME`. Explicit rather
/// than `std::getenv` — but see the module header for why that alone does not
/// make this safe to run against a real database.
/// @return One report per org, or nullopt when the org listing failed.
export auto run(db::connection& conn, const identity::env_lookup& env) -> std::optional<std::vector<org_report>>;

/// @brief Diagnose (and repair) one org.
///
/// Exposed so a single org's diagnosis can be driven directly.
/// @param conn An open connection.
/// @param env Environment lookup callable.
/// @param org The org to diagnose.
/// @return The report.
export auto diagnose(db::connection& conn, const identity::env_lookup& env, const identity::workspace& org) -> org_report;

/// @brief Render `workspace doctor --json`.
///
/// One object for the whole run, `{"orgs":[...]}`, newline-terminated. The
/// per-org key is `issues_repaired`, NOT `issues` — the wire name is kept even
/// though the array holds unrepaired entries too.
///
/// An empty database is `{"orgs":[]}\n`, exit 0.
/// @param reports Every org's report.
/// @return The JSON, newline-terminated.
export auto doctor_json(std::span<const org_report> reports) -> std::string;

/// @brief Render `workspace doctor` (no `--json`).
///
/// Per org: every issue as `<kind>: <detail>`, then a single summary line —
/// `org:<slug> ok` when nothing was found, `org:<slug> repaired N issues`
/// otherwise.
///
/// An empty database produces ZERO BYTES, where `--json` produces
/// `{"orgs":[]}`. The two modes disagree, and both are pinned.
/// @param reports Every org's report.
/// @return The complete stdout payload.
export auto doctor_text(std::span<const org_report> reports) -> std::string;

/// @brief The refusal `routing show` / `routing build` / `regenerate` emit when
/// no org exists — or when a named one does not.
///
/// Shared by three leaves, byte-identical in all of them and in both text and
/// `--json` modes (there is no JSON error envelope anywhere in this verb
/// family). `doctor` deliberately does NOT emit it.
/// @return The line, newline-terminated.
export auto no_orgs_error() -> std::string;

/// @brief The refusal those same three leaves emit when several orgs exist and
/// no workspace was named.
/// @return The line, newline-terminated.
export auto ambiguous_orgs_error() -> std::string;

} // namespace planar::engine::workspace::doctor

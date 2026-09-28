/// @file lint.cppm
/// @brief `planar.engine.workbench.lint` — validate workbench Markdown
/// without syncing (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of zig/src/engine/workbench/lint.zig.
///
/// `lint` is the operator-facing face of
/// `planar.engine.workbench.parse::diagnose`: it walks a file or a tree,
/// runs the SAME check `pull` runs, and turns each rejection into a coded,
/// hinted diagnostic. That shared gate is the point — a file `lint` passes
/// is a file `pull` will accept.
///
/// Two severities, and BOTH make the verb exit non-zero (oracle-confirmed:
/// a warnings-only run still exits 1):
///
///   error   — the file does not parse. One per file; the walk moves on.
///   warning — the file parses, but its `anchor_plan_id` is missing, zero,
///             or names no existing plan. This is the only check that
///             touches the database.
module;

export module planar.engine.workbench.lint;

import std;
import planar.db;

namespace planar::engine::workbench::lint {

/// @brief Diagnostic severity. Rendered as the bare word in
/// `error[<code>]:` / `warning[<code>]:` and in the `--json` `severity`
/// field.
export enum class severity : std::uint8_t { error, warning };

/// @brief The operator-visible spelling of a severity.
/// @param value The severity.
/// @return `"error"` or `"warning"`.
export auto severity_name(severity value) -> std::string_view;

/// @brief One diagnostic. Field order matches the `--json` record.
export struct issue {
  std::string   path;                    ///< The file, as the caller named it.
  std::size_t   line  = 1;               ///< 1-based; synthetic for missing-field cases (see parse.cppm).
  enum severity level = severity::error; ///< Error (the file does not parse) or warning (it does, but dangles).
  std::string   code;                    ///< e.g. `malformed_frontmatter`, `anchor_plan_not_found`.
  std::string   message;                 ///< One sentence, no trailing period-plus-detail.
  std::string   hint;                    ///< The remedy; may be empty.
};

/// @brief What one run found.
export struct result {
  std::size_t        files_scanned = 0; ///< How many `.md` files were read.
  std::size_t        errors        = 0; ///< Files that did not parse.
  std::size_t        warnings      = 0; ///< Files that parsed but whose anchor plan is missing.
  std::vector<issue> issues;            ///< Every diagnostic, ordered by path.
};

/// @brief Failure surface.
export enum class lint_error : std::uint8_t {
  not_found,     ///< The target path does not exist.
  invalid_input, ///< The target is a file that is not `.md`, or is neither file nor directory.
  query_failed,  ///< SQLite refused an operation.
};

/// @brief Lint one Markdown file or one directory tree.
///
/// A directory is walked recursively for `*.md` and the results are sorted
/// by path, so the output is stable across filesystems. A FILE target must
/// end in `.md` — the oracle refuses anything else with `invalid_input`
/// (exit 2) rather than skipping it, which is a different exit code from an
/// absent path (exit 1). Both probed.
/// @param conn The database connection (for the anchor-plan existence check).
/// @param target The file or directory to lint.
/// @return The result, or the failure.
export auto run(db::connection& conn, const std::filesystem::path& target) -> std::expected<result, lint_error>;

} // namespace planar::engine::workbench::lint

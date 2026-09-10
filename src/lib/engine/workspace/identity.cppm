/// @file identity.cppm
/// @brief `planar.engine.workspace.identity` — org-association selection, the
/// per-workspace state-directory layout, and the root-guidance symlink install
/// (plan 996, task 6110).
///
/// Behavior-preserving port (D2) of zig/src/engine/identity/workspace.zig.
///
/// ## What a "workspace" is here
///
/// An `associations` row with `kind = 'org'`. Every `planar workspace *` leaf
/// starts by selecting exactly one of them, then derives a state directory
/// under `$PLANAR_HOME/workspaces/<org_id>/` holding four canonical files:
///
///     AGENTS.md           the generated routing guidance
///     routing-table.json  the machine-readable routing table
///     config.toml         workspace configuration
///     README.md           operator prose
///
/// The polyrepo workspace ROOT (a different directory, recorded in the
/// association's `config_json.root_path`) then gets `AGENTS.md` and `CLAUDE.md`
/// installed as symlinks pointing at that ONE generated `AGENTS.md`. Both names
/// point at the same target; there is no separate CLAUDE.md content.
///
/// ## HOME safety, and a correction worth reading
///
/// `planar_home` reads `PLANAR_HOME` (with `~` expansion), falling back to
/// `$HOME/.planar` — unlike `engine/local`, which never consults `PLANAR_HOME`
/// at all. As everywhere else in this tree, the lookup is an explicit CALLABLE
/// rather than `std::getenv`, so this module cannot find a real home on its own.
///
/// But redirecting the environment is NOT sufficient protection for this
/// bucket, and this was established by probing rather than assumed. **`workspace
/// doctor` does not write to the current working directory.** It writes to the
/// association's recorded `root_path`. Run from an unrelated directory, doctor
/// still installed `AGENTS.md` / `CLAUDE.md` into the root recorded at
/// `workspace init` time. So cwd isolation buys nothing here: the protection is
/// a scratch `PLANAR_DB`, because the destination comes out of the DATABASE.
/// Against a real database, doctor writes into the operator's real workspace
/// root from any cwd whatsoever.
///
/// ## Selection, and the refusal that lies
///
/// With no target, `resolve_org` requires EXACTLY one org to exist: zero is
/// `not_found`, two or more is `ambiguous`. With a target, an optional `org:`
/// prefix is stripped and the remainder is tried as an id if it parses as an
/// integer, otherwise as a slug.
///
/// The oracle-captured surprise: an UNMATCHED slug on a populated database
/// emits the SAME message as an empty database:
///
///     error: no org associations registered; create one with
///     `planar workspace init`
///
/// Verified with org
/// `acme` present and `routing show nosuch --json` supplied. Both map onto
/// `not_found`, and the message is written for the empty case only. Preserved,
/// and named here so nobody "fixes" the wording without realising it changes an
/// existing capture.
///
/// ## `root_path` parsing fails SOFT
///
/// `config_json` is parsed for a non-empty string `root_path`. A malformed or
/// non-object `config_json`, an absent key, or a non-string value all yield
/// `std::nullopt` rather than an error — the row is still a usable workspace,
/// it just has no root to install guidance into.

module;

export module planar.engine.workspace.identity;

import std;
import planar.db;

namespace planar::engine::workspace::identity {

/// @brief The strategy string reported after a successful symlink install.
export inline constexpr std::string_view strategy_symlink = "symlink";

/// @brief The strategy string reported when any install fell back to a copy.
export inline constexpr std::string_view strategy_copy = "copy";

/// @brief The two root-level guidance filenames, in install order.
///
/// BOTH point at the same generated `AGENTS.md`; `CLAUDE.md` has no separate
/// content. The order is observable — `doctor` reports one `fix` per dirty name
/// in this sequence.
export inline constexpr std::array<std::string_view, 2> link_names{"AGENTS.md", "CLAUDE.md"};

/// @brief One org association.
export struct workspace {
  std::int64_t               id = 0;    ///< The `associations.id`.
  std::string                slug;      ///< The `associations.slug`.
  std::string                name;      ///< The `associations.name`.
  std::optional<std::string> root_path; ///< From `config_json.root_path`; absent when unusable.
};

/// @brief Why an org could not be selected.
export enum class resolve_error {
  not_found,    ///< No org at all, OR the named one does not exist. See the header.
  ambiguous,    ///< More than one org and no target given.
  query_failed, ///< The database refused.
};

/// @brief The four canonical files of a workspace's state directory.
export struct layout {
  std::int64_t          org_id = 0;    ///< The org this layout belongs to.
  std::filesystem::path dir;           ///< `$PLANAR_HOME/workspaces/<org_id>`.
  std::filesystem::path agents_md;     ///< `<dir>/AGENTS.md`.
  std::filesystem::path routing_table; ///< `<dir>/routing-table.json`.
  std::filesystem::path config_toml;   ///< `<dir>/config.toml`.
  std::filesystem::path readme_md;     ///< `<dir>/README.md`.
};

/// @brief An environment lookup: name -> value, or nullopt when unset.
export using env_lookup = std::function<std::optional<std::string>(std::string_view)>;

/// @brief Select exactly one org association.
///
/// See the module header for the empty-target rules and for why an unmatched
/// slug reports `not_found` with the "no org associations registered" wording.
/// @param conn An open connection.
/// @param target The `[workspace]` positional: an id, a slug, either with an
/// optional `org:` prefix, or nullopt/blank for "the only one". Surrounding
/// whitespace is trimmed, and a whitespace-only target is treated as absent.
/// @return The selected workspace, or why not.
export auto resolve_org(db::connection& conn, std::optional<std::string_view> target) -> std::expected<workspace, resolve_error>;

/// @brief Every org association, in `id` order.
///
/// `workspace doctor` uses this rather than resolve_org(): it reports on ALL
/// orgs and never refuses on an empty or ambiguous database.
/// @param conn An open connection.
/// @return The orgs, or nullopt when the query failed.
export auto list_orgs(db::connection& conn) -> std::optional<std::vector<workspace>>;

/// @brief Extract `root_path` out of an association's `config_json`.
///
/// Fails SOFT: malformed JSON, a non-object, an absent key, a non-string value
/// and an empty string all yield nullopt rather than an error.
/// @param config_json The raw column value; may be empty.
/// @return The root path, or nullopt.
export auto parse_root_path(std::string_view config_json) -> std::optional<std::string>;

/// @brief Resolve `$PLANAR_HOME`, expanding a leading `~`.
///
/// `PLANAR_HOME` when set and NON-EMPTY (an empty value falls through), else
/// `$HOME/.planar`, else failure. `~` alone expands to `$HOME`; `~/x` to
/// `$HOME/x`; a `~` anywhere else is left alone.
/// @param env Environment lookup callable.
/// @return The resolved directory, or nullopt when neither variable is usable.
export auto planar_home(const env_lookup& env) -> std::optional<std::filesystem::path>;

/// @brief Compute a workspace's state-directory layout WITHOUT creating it.
/// @param env Environment lookup callable.
/// @param org_id The org; must be positive.
/// @return The layout, or nullopt when the home could not be resolved or the id
/// is not positive.
export auto load_layout(const env_lookup& env, std::int64_t org_id) -> std::optional<layout>;

/// @brief Compute the layout AND create its directory.
/// @param env Environment lookup callable.
/// @param org_id The org.
/// @return The layout, or nullopt when the home could not be resolved or the
/// directory could not be created.
export auto ensure_layout(const env_lookup& env, std::int64_t org_id) -> std::optional<layout>;

/// @brief Which of `link_names` are missing or point somewhere else.
///
/// A name is DIRTY when `readlink` fails for ANY reason — including "it is a
/// real file an operator wrote by hand" — or when it resolves to something
/// other than `target`.
///
/// The Zig original writes this as an `if`/`else` whose two arms are IDENTICAL
/// (both append the name), which reads as though the two cases were meant to
/// differ and then did not. This port collapses them, and says so here so the
/// next reader does not go looking for the missing distinction.
/// @param workspace_root The directory holding the guidance files.
/// @param target The `AGENTS.md` every link should point at.
/// @return The dirty names, in `link_names` order.
export auto dirty_links(const std::filesystem::path& workspace_root, const std::filesystem::path& target)
    -> std::vector<std::string>;

/// @brief Install both guidance links, creating `workspace_root` if needed.
///
/// Symlink first; on a filesystem that refuses one (permission denied, a
/// read-only mount) the target's BYTES are copied instead and the reported
/// strategy for the whole call becomes `copy`. A single fallback makes the
/// whole install report `copy`, even if the other link succeeded as a symlink.
///
/// Writes go to a dotted `.<name>.symlink.tmp` beside the destination and are
/// renamed into place, so an interrupted install cannot leave a half-written
/// guidance file where a working one was.
///
/// An ALREADY-CORRECT link is left completely alone — not removed and
/// recreated — which is what makes doctor idempotent.
///
/// ## Two break-probe survivors, and why they are not test defects
///
/// Both were run and both survived, and the analysis is recorded here rather
/// than papered over:
///
///   - Deleting the already-correct early return above. The link is then
///     removed and recreated, and the END STATE is byte-identical, so no
///     assertion over the filesystem can see it. It is a churn guard, not a
///     correctness guard, and it is unobservable through this module's API by
///     construction. Kept because doctor runs on every invocation and
///     needlessly rewriting an operator's guidance links on each one is a real
///     (if invisible) cost.
///
///   - Hardcoding the returned strategy to `symlink`. The `copy` arm only fires
///     on a filesystem that REFUSES symlinks — a read-only mount, or Windows
///     without developer mode — and no such filesystem is available to a test
///     running under `temp_directory_path()`. The branch is unreachable on the
///     test platform, in the same way the previous cycle's Wilson clamp was
///     unreachable across the first 200,000 sample counts. Forcing it would
///     mean asserting on a permissions failure, which is itself unreliable
///     under root.
/// @param workspace_root The directory to install into.
/// @param value The layout whose `agents_md` is the link target.
/// @return `symlink` or `copy`, or nullopt when the install failed.
export auto install_symlinks(const std::filesystem::path& workspace_root, const layout& value) -> std::optional<std::string_view>;

/// @brief Remove both guidance links from `workspace_root`.
///
/// An absent link is not an error.
/// @param workspace_root The directory to clean.
/// @return True on success.
export auto remove_symlinks(const std::filesystem::path& workspace_root) -> bool;

} // namespace planar::engine::workspace::identity

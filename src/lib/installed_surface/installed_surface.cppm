/// @file installed_surface.cppm
/// @brief `planar.installed_surface` — the layer-1 manifest-owned
/// installed-vendor-projection classifier (plan 996, task 6357).
///
/// Behavior-preserving port (D2) of zig/src/engine/installedsurface.zig's
/// `status`.
///
/// ## Why this is a LAYER-1 module rather than part of `engine_health`
///
/// `planar health`'s handler folds this classifier into every run, and
/// `engine_health` is a layer-2 bucket. This module touches NO SQLite handle
/// — it is manifest JSON plus a filesystem walk — so there is no `db` edge
/// pulling it toward `engine_health` directly; it was written standalone and
/// promoted straight to layer 1, the same "next consumer is a different
/// layer-2 bucket" shape `scope_ref` (D19), `json_dom`, `process` and
/// `introspection_preview` (decision 981) each record for themselves. A
/// direct `engine_health -> engine_installedsurface` edge is exactly the
/// `engine_* -> engine_*` shape D15/D18 FATAL on at configure time.
///
/// ## The versioned install manifest is the ONLY ownership authority
///
/// Directory discovery exists solely to label destination-only entries
/// `unmanaged`; it never promotes such an entry into the managed repair set.
///
/// Freshness is a plain existence + byte/symlink comparison against the
/// staged authority, not a semantic digest match. The staged authority is
/// `$PLANAR_HOME/skills/planar/` (the skill, a directory),
/// `$PLANAR_HOME/agents/planar-<role>.md` (Markdown, Copilot and OpenCode
/// agents; OpenCode's installed form is derived from it by `derive_opencode`)
/// and `$PLANAR_HOME/codex-agents/planar-<role>.toml` (Codex agents).
///
/// ## The nine roots
///
/// Planar places into nine vendor roots (`installed_roots`), each tagged with
/// the vendors that read it. A root takes part only when at least one of its
/// vendors is present on the host, with the same presence markers
/// `install.sh`'s `vendor_present` uses. The shared `~/.agents/skills` root is
/// recorded in the manifest under the vendor name `shared`: it is placed once
/// however many of its four owners are present, so no single vendor owns it.
///
/// ## Manifest versions
///
/// Version 2 is current. A version 1 manifest (the retired four-vendor,
/// per-file layout, including the interim one that recorded placements only
/// in `extras`) reads as `manifest_state::legacy` with a reinstall repair
/// command; it never yields rows and never fails `planar health`.
///
/// Caveat inherited from the oracle (Zig question 880): freshness depends on
/// the staged tree under `$PLANAR_HOME` being retained on disk post-install
/// as the comparison authority. If it is pruned, every managed row reads
/// `stale` ("staged projection is unavailable").
module;

export module planar.installed_surface;

import std;

namespace planar::installed_surface {

/// @brief The six vendors this tree knows how to classify, in the installer's
/// `VENDOR_NAMES` order — that order is the JSON/text wire order for `vendors`.
export inline constexpr std::array<std::string_view, 6> supported_vendors{"claude", "codex",       "copilot",
                                                                          "gemini", "antigravity", "opencode"};

/// @brief The manifest row vendor naming a root that several vendors read
/// (`~/.agents/skills`). It is valid on a row but is not a selectable vendor.
export inline constexpr std::string_view shared_vendor = "shared";

/// @brief The install-manifest schema version this module accepts. Version 1
/// reads as `manifest_state::legacy`; any other value is `unsupported`.
export inline constexpr std::uint32_t manifest_version = 2;

/// @brief Per-projection freshness classification.
export enum class state : std::uint8_t {
  fresh,     ///< Staged and installed projections agree.
  stale,     ///< Both exist but disagree, or the staged source is unavailable.
  missing,   ///< The manifest owns this row but the installed path is absent.
  unmanaged, ///< A destination entry with no manifest row.
};

/// @brief Coarse health of the install manifest file itself.
export enum class manifest_state : std::uint8_t {
  current,     ///< Present, parses, and matches `manifest_version`.
  missing,     ///< No `install-manifest.json` and no legacy stamp either.
  legacy,      ///< No manifest, but a `.planar-install` ownership stamp exists.
  invalid,     ///< Present but fails to parse or fails structural validation.
  unsupported, ///< Present, parses, but its `version` is not `manifest_version`.
};

/// @brief Whether a vendor is named in the manifest's `vendors` list.
export enum class vendor_state : std::uint8_t {
  selected,   ///< The manifest opted this vendor in.
  unselected, ///< The manifest never named this vendor.
};

/// @brief One vendor's selection state and managed-row count.
export struct vendor_status {
  std::string  vendor;                                   ///< One of `supported_vendors`.
  vendor_state status        = vendor_state::unselected; ///< Selected iff the manifest names it.
  std::size_t  managed_count = 0;                        ///< Manifest rows for this vendor; 0 when unselected.
};

/// @brief One classified projection row — either manifest-owned or
/// discovered-unmanaged.
export struct projection_status {
  std::string                vendor;                                   ///< The owning vendor.
  std::string                kind;                                     ///< `skill` or `agent`.
  std::string                name;                                     ///< The projection's name.
  std::string                staged_path;                              ///< Empty for an unmanaged row.
  std::string                installed_path;                           ///< Where it lives (or would live) on disk.
  std::string                install_kind;                             ///< `copy`, `link`, or `unmanaged`.
  installed_surface::state   status = installed_surface::state::stale; ///< The classification.
  std::string                reason;                                   ///< Human-readable justification.
  std::optional<std::string> repair_command;                           ///< Set only when repair applies.
};

/// @brief Aggregate counts across every classified projection.
export struct summary {
  std::size_t fresh              = 0; ///< Managed rows that match the staged authority.
  std::size_t stale              = 0; ///< Managed rows that disagree with the staged authority.
  std::size_t missing            = 0; ///< Managed rows whose installed path is absent.
  std::size_t unmanaged          = 0; ///< Destination entries with no manifest row.
  std::size_t unselected_vendors = 0; ///< Vendors the manifest never names.
};

/// @brief Inputs to `status`.
export struct options {
  std::string                planar_home; ///< `$PLANAR_HOME`; hosts `install-manifest.json`.
  std::string                home;        ///< `$HOME`; hosts every vendor root except the Codex agents root.
  std::string                codex_home;  ///< `$CODEX_HOME` (default `$HOME/.codex`); hosts the Codex agents root.
  std::optional<std::string> vendor;      ///< Restrict to one vendor (or `shared`-root readers) when set.
  bool codex_home_set = false;            ///< `$CODEX_HOME` was set in the environment; with `~/.codex/` it marks Codex present.
};

/// @brief One of the nine vendor roots Planar places into, resolved against
/// the homes in `options`.
export struct installed_root {
  std::string              path;            ///< Absolute directory.
  std::string              kind;            ///< `skill` or `agent`.
  bool                     directory_shape; ///< Skill roots hold a directory per skill; agent roots hold files.
  bool                     derived;         ///< Installed bytes are derived from the staged file (OpenCode).
  std::vector<std::string> vendors;         ///< Vendors that read this root, in `supported_vendors` order.
  bool                     present;         ///< At least one tagged vendor is present on this host.
};

/// @brief The nine vendor roots, in the installer's placement order.
/// @param opts The home directories; `codex_home_set` feeds Codex presence.
/// @return Exactly nine entries, each tagged with its vendors and presence.
export auto installed_roots(const options& opts) -> std::vector<installed_root>;

/// @brief Derive the OpenCode form of a staged agent: the frontmatter reduced
/// to `description` and `mode: subagent`, the body unchanged. Byte-equal to
/// `ownership.sh`'s `opencode_derive`.
/// @param staged The staged `.md` bytes.
/// @return The derived bytes, or `std::nullopt` when the file does not open
/// with `---`, has no description, or its frontmatter never closes.
export auto derive_opencode(std::string_view staged) -> std::optional<std::string>;

/// @brief The whole classification result.
export struct status_result {
  installed_surface::manifest_state manifest_status = manifest_state::missing; ///< The manifest file's own health.
  std::optional<std::uint32_t>      manifest_version_seen; ///< The manifest's own `version` field, when parsed.
  std::optional<std::string>        build_id;              ///< The manifest's `build_id`, when parsed.
  std::optional<std::string>        install_mode;          ///< The manifest's `install_mode`, when parsed.
  std::string                       manifest_path;         ///< Where `install-manifest.json` was looked for.
  std::optional<std::string>        reason;         ///< Human-readable justification, set whenever the manifest is not `current`.
  std::optional<std::string>        repair_command; ///< The full-reinstall command; set whenever repair applies.
  std::vector<vendor_status>        vendors;        ///< One entry per considered vendor.
  std::vector<projection_status>    projections;    ///< Every classified row, managed and unmanaged.
  installed_surface::summary        summary;        ///< The aggregate counts.
};

/// @brief Why `status` refused outright, as opposed to reporting a degraded
/// classification (which is a SUCCESSFUL `status_result`, not an error).
export enum class status_error : std::uint8_t {
  invalid_input,  ///< One of `planar_home` / `home` / `codex_home` is empty.
  invalid_vendor, ///< `options.vendor` names something outside `supported_vendors`.
};

/// @brief Classify the installed vendor projections against the versioned
/// install manifest.
///
/// A missing, legacy, invalid or unsupported manifest is NOT an error — it
/// is a `status_result` whose `manifest_status` records which of those four
/// it is, with every vendor reported `unselected` and no projections. Only
/// `invalid_input` / `invalid_vendor` refuse outright.
/// @param opts The three home directories and an optional vendor filter.
/// @return The classification, or why the call refused.
export auto status(const options& opts) -> std::expected<status_result, status_error>;

} // namespace planar::installed_surface

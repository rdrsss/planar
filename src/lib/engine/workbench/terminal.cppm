/// @file terminal.cppm
/// @brief `planar.engine.workbench.terminal` — which (kind, status) pairs
/// are excluded from the workbench write set (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of zig/src/engine/workbench/terminal.zig.
///
/// One predicate — `is_filtered_str` — is what `push`, `restore` and `gc`
/// all consume. Everything above it exists to make that predicate
/// exhaustive: each kind's status enum mirrors its migration CHECK
/// constraint, and each `classify_*` switches over every variant, so a
/// future migration that adds a status forces a compile error here rather
/// than silently classifying the new value as "active".
///
/// ## The two modes
///
///   `failures` (default) — excludes FAILURE terminals only: abandoned,
///     cancelled, superseded, withdrawn, retired, wontfix.
///   `all` — additionally excludes SUCCESS terminals: done, answered,
///     verified.
///
/// Active statuses are never excluded under either mode.
///
/// ## Unknown values pass through
///
/// `is_filtered_str` returns unset — NOT `false` — when either string is
/// unrecognized, so each caller decides. They all decide "keep": `push`
/// writes the entity, `gc` counts it as kept. Oracle-consistent, and it
/// means a schema addition degrades to "does nothing" rather than to
/// "deletes files".
module;

export module planar.engine.workbench.terminal;

import std;

namespace planar::engine::workbench::terminal {

/// @brief The six entity kinds the workbench projects onto the filesystem.
export enum class kind : std::uint8_t { plan, task, decision, question, test_scenario, artifact };

/// @brief The operator-selected filter breadth.
export enum class mode : std::uint8_t { failures, all };

/// @brief Where a (kind, status) pair sits on the terminal axis.
export enum class classification : std::uint8_t {
  active,           ///< Live work; never excluded.
  success_terminal, ///< Completed successfully; excluded only under `all`.
  failure_terminal, ///< Abandoned/cancelled/superseded/withdrawn/retired/wontfix; always excluded.
};

/// @brief Parse an entity kind.
///
/// Accepts BOTH `test_scenario` (the table's own name) and `scenario` (the
/// alias the sync layer's entity stream carries), because both reach this
/// function from different callers.
/// @param text The kind string.
/// @return The kind, or unset when unrecognized.
export auto kind_from_string(std::string_view text) -> std::optional<kind>;

/// @brief Parse a filter mode.
/// @param text `"failures"` or `"all"`.
/// @return The mode, or unset when unrecognized.
export auto mode_from_string(std::string_view text) -> std::optional<mode>;

/// @brief The operator-visible spelling of a mode. Appears verbatim in
/// `workbench push`'s `(mode=failures)` summary and in the `filter_mode`
/// JSON field.
/// @param value The mode.
/// @return `"failures"` or `"all"`.
export auto mode_to_string(mode value) -> std::string_view;

/// @brief Whether an entity should be EXCLUDED from the write set.
/// @param value The classification.
/// @param filter The active mode.
/// @return `true` when the entity is excluded.
export auto is_filtered(classification value, mode filter) -> bool;

/// @brief String-keyed adapter — the one entry point the sync, restore and
/// gc paths use, since they carry kind and status as text from SQLite.
/// @param kind_text The entity kind.
/// @param status_text The entity status.
/// @param filter The active mode.
/// @return Whether the entity is excluded, or unset when either string is
/// unrecognized (see this file's header — every caller treats unset as
/// "keep").
export auto is_filtered_str(std::string_view kind_text, std::string_view status_text, mode filter) -> std::optional<bool>;

/// @brief Classify one (kind, status) pair.
///
/// Exposed so the exhaustiveness tests can walk every status of every kind
/// without going through the string layer.
/// @param entity_kind The entity kind.
/// @param status_text The entity status.
/// @return The classification, or unset when the status is not valid for
/// that kind.
export auto classify(kind entity_kind, std::string_view status_text) -> std::optional<classification>;

/// @brief Every status string valid for `entity_kind`, in the order the
/// migration's CHECK constraint lists them.
///
/// Exposed for the exhaustiveness test: it walks this list and asserts
/// `classify` answers for every entry, so a status added to `classify`
/// without being added here (or the reverse) fails a test rather than
/// silently changing filter behavior.
/// @param entity_kind The entity kind.
/// @return The status list.
export auto statuses_of(kind entity_kind) -> std::span<const std::string_view>;

} // namespace planar::engine::workbench::terminal

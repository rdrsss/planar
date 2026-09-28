/// @file feature.cppm
/// @brief `planar.engine.workbench.feature` — the workbench directory
/// layout: where a feature's tree lives and what each file is called
/// (plan 996, task 6037).
///
/// Behavior-preserving port (D2) of zig/src/engine/workbench/feature.zig.
///
/// ## Layout, oracle-captured
///
/// A real `workbench push` against a scratch root produced exactly this:
///
///     <root>/project_demo/p1-demo-feature/
///       README.md                          anchor plan
///       1-tech-spec-auth.md                artifacts, at the feature ROOT
///       plans/<slug>.md                    child plans
///       tasks/<repo-slug|cross>/<id>-<slug>.md
///       decisions/<id>-<slug>.md
///       questions/<id>-<slug>.md
///       scenarios/<id>-<slug>.md
///       .sync                              the manifest mirror
///
/// Note the artifact path: at the feature-dir ROOT, not under `artifacts/`.
/// The Zig source carries a comment saying an older revision wrote them
/// under `artifacts/`; the SHIPPED behavior is the root, and that is what
/// the oracle run confirms.
///
/// ## Path confinement
///
/// Every segment that comes from operator- or external-system-controlled
/// data is sanitized before it becomes a path component: `:`, `/`, `\` and
/// NUL become `_`, and a segment that is exactly `.` or `..` becomes `_`.
/// That is what keeps an association slug like `org:eng` or an external
/// plan key like `owner/repo#1` from escaping the root — `..`/`..`/`target`
/// renders as the single literal segment `.._.._target`. This module is the
/// only place those rules live.
module;

export module planar.engine.workbench.feature;

import std;

namespace planar::engine::workbench::feature {

/// @brief The absolute feature directory for an anchor plan:
/// `<root>/<safe-assoc>/<safe-key>-<safe-slug>`.
///
/// An EMPTY `assoc_slug` collapses the association level away entirely
/// (`<root>/<key>-<slug>`), which is the global-scope case.
/// @param root The resolved workbench root.
/// @param assoc_slug The owning association's slug, or empty for global.
/// @param plan_key The plan's external id, or its `p<id>` fallback.
/// @param plan_slug The plan's slug.
/// @return The absolute directory path.
export auto feature_dir(std::string_view root, std::string_view assoc_slug, std::string_view plan_key, std::string_view plan_slug)
    -> std::string;

/// @brief The manifest-stored, root-relative path for one file:
/// `<safe-assoc>/<safe-key>-<safe-slug>/<rel_path>`.
///
/// This is the exact string stored in `workbench_sync_state.file_path` and
/// reported in every `workbench *` payload, so it is forward-slashed and
/// root-relative regardless of platform.
/// @param assoc_slug The owning association's slug, or empty for global.
/// @param plan_key The plan's external id, or its `p<id>` fallback.
/// @param plan_slug The plan's slug.
/// @param rel_path The feature-relative path, forward-slashed.
/// @return The root-relative stored path.
export auto stored_path(std::string_view assoc_slug, std::string_view plan_key, std::string_view plan_slug,
                        std::string_view rel_path) -> std::string;

/// @brief An artifact's workbench filename: `<id>-<slug>.md`.
///
/// When the title slugifies to nothing (or to the literal `untitled`) the
/// KIND is used instead, with `_` rewritten to `-` — so an untitled
/// `tech_spec` with id 3 is `3-tech-spec.md`.
/// @param id The artifact id.
/// @param title The artifact title.
/// @param kind The artifact kind, e.g. `tech_spec`.
/// @return The filename.
export auto artifact_filename(std::int64_t id, std::string_view title, std::string_view kind) -> std::string;

/// @brief Map a storage alias onto its canonical entity kind.
///
/// The only alias is `test_scenario` -> `scenario`; everything else passes
/// through unchanged.
/// @param kind The raw kind.
/// @return The canonical kind.
export auto canonical_entity_kind(std::string_view kind) -> std::string_view;

/// @brief Lowercase, collapse every non-alphanumeric run to one `-`, trim
/// `-` from both ends, truncate to 60 characters, then re-trim a trailing
/// `-` the truncation may have exposed.
///
/// Returns `untitled` when the result would be empty. The 60-character cap
/// and the SECOND trim are both load-bearing: without the re-trim a title
/// that happens to break on a separator yields a filename ending in `-`.
/// @param title The title to slugify.
/// @return The slug.
export auto slugify(std::string_view title) -> std::string;

} // namespace planar::engine::workbench::feature

/// @file templates.cppm
/// @brief `planar.engine.templates` — umbrella re-export for the
/// engine/templates bucket (plan 996, task 6190).
///
/// This bucket is the RENDERING half of the template plane. The RESOLUTION
/// half — the three-level `<root>/<set>` → `<root>/default` → embedded
/// fallback chain, plus the disk/embedded enumerators — already lives in
/// `planar.engine.config.templates` (task 6032) and is NOT re-exported
/// here: a consumer that needs both imports both, and folding a
/// `engine_config` symbol into this namespace would hide which bucket owns
/// the loader.
///
/// `planar.json_dom` is likewise NOT re-exported. It is a LAYER-1 base
/// library that this bucket happens to be the largest consumer of, not a
/// part of it, and both halves of the template plane import it directly.

export module planar.engine.templates;

export import planar.engine.templates.context;
export import planar.engine.templates.render;
export import planar.engine.templates.validate;
export import planar.engine.templates.extract;
export import planar.engine.templates.builder;
export import planar.engine.templates.output;

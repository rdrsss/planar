/// @file workbench.cppm
/// @brief `planar.engine.workbench` — umbrella re-export for the workbench
/// bucket (plan 996, task 6037). Same pattern as
/// `planar.engine.planning` and `planar.engine.ingest`.
///
/// Import this to get the whole bucket; import a leaf module directly when
/// only one surface is needed (the layer-3 handlers do the latter, so a
/// change to `gc` does not recompile the `lint` handler).
export module planar.engine.workbench;

export import planar.engine.workbench.feature;
export import planar.engine.workbench.fsutil;
export import planar.engine.workbench.gc;
export import planar.engine.workbench.lint;
export import planar.engine.workbench.manifest;
export import planar.engine.workbench.parse;
export import planar.engine.workbench.render;
export import planar.engine.workbench.render_cli;
export import planar.engine.workbench.root;
export import planar.engine.workbench.sync;
export import planar.engine.workbench.terminal;

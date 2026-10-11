/// @file config.cppm
/// @brief `planar.engine.config` — single import point re-exporting the
/// whole `src/engine/config` surface (tech-spec § File-level tree:
/// "config.cppm; config (Glaze TOML), templates, templates_embed.cppm.in,
/// init, policy"). The `policy` module is the per-scope policy keys of plan
/// 1142; this umbrella re-exports every landed module, matching
/// `src/engine/identity/identity.cppm`'s same pattern.
module;

export module planar.engine.config;

export import planar.engine.config.toml;
export import planar.engine.config.policy;
export import planar.engine.config.effective;
export import planar.engine.config.templates_embed;
export import planar.engine.config.templates;
export import planar.engine.config.init;
export import planar.engine.config.queue;

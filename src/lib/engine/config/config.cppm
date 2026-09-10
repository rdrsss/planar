/// @file config.cppm
/// @brief `planar.engine.config` — single import point re-exporting the
/// whole `lib/engine/config` surface (tech-spec § File-level tree:
/// "config.cppm; config (Glaze TOML), templates, templates_embed.cppm.in,
/// init, policy"). `policy` is not part of this task's scope (plan 996
/// task 6032: "Config plane, templates #embed + overrides, init verb" —
/// no policy-bucket task exists yet); this umbrella re-exports exactly the
/// landed subset, matching `lib/engine/identity/identity.cppm`'s same
/// pattern.
module;

export module planar.engine.config;

export import planar.engine.config.toml;
export import planar.engine.config.effective;
export import planar.engine.config.templates_embed;
export import planar.engine.config.templates;
export import planar.engine.config.init;

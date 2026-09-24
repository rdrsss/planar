/// @file identity.cppm
/// @brief `planar.engine.identity` — single import point re-exporting the
/// whole `src/engine/identity` surface (tech-spec § File-level tree:
/// "identity.cppm; scope, association, project, workspace"). `project` and
/// `workspace` are not yet ported (plan 996 task cpp-scope-assoc's scope is
/// scope resolution + the cross-scope guard + the association surface it
/// needs); this umbrella re-exports only what exists so far, matching
/// `lib/cli/cli.cppm`'s pattern of re-exporting exactly the landed subset.
module;

export module planar.engine.identity;

export import planar.engine.identity.scope;
export import planar.engine.identity.association;

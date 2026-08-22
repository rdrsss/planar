/// @file cli.cppm
/// @brief `planar.cli` — single import point re-exporting the whole `lib/cli`
/// surface (tech-spec § File-level tree: "cli.cppm; cmd, parser, flag,
/// help, completion, validate, schema — the etcli port").
///
/// `validate` (authoring-time tree-shape lint) and `schema` (the JSON
/// catalog `tools/cli_usage_lint` consumes) are separate tasks (schema is
/// explicitly task 6029) and are not re-exported here yet — this facade
/// grows an `export import planar.cli.schema;` line when that module
/// lands; nothing about this shape needs to change to accommodate it,
/// which is the "introspectable enough for a later pass" requirement this
/// task's brief calls out (`all_nodes`/`all_leaves`/
/// `collect_inherited_flags` in cmd.cppm are exactly the walk a schema
/// emitter needs).
module;

export module planar.cli;

export import planar.cli.flag;
export import planar.cli.cmd;
export import planar.cli.error;
export import planar.cli.parser;
export import planar.cli.help;
export import planar.cli.completion;

/// @file cli.cppm
/// @brief `planar.cli` — single import point re-exporting the whole `lib/cli`
/// surface (tech-spec § File-level tree: "cli.cppm; cmd, parser, flag,
/// help, completion, validate, schema — the etcli port").
///
/// `validate` (authoring-time tree-shape lint) is a separate, still-open
/// task and is not re-exported here yet. `schema` (task 6029, the JSON
/// catalog `tools/cli_usage_lint` consumes) is re-exported below —
/// `all_nodes`/`all_leaves`/`collect_inherited_flags` in cmd.cppm turned
/// out to be exactly the walk the emitter needed, so no shape change was
/// required to land it. `version`, `output`, and `exit` (task
/// cpp-cli-output-logging) round out the dispatch-adjacent surface: build
/// metadata + `planar version` rendering, JSON/text emission helpers, and
/// the domain-error -> process-exit-code table.
module;

export module planar.cli;

export import planar.cli.flag;
export import planar.cli.cmd;
export import planar.cli.error;
export import planar.cli.parser;
export import planar.cli.help;
export import planar.cli.completion;
export import planar.cli.schema;
export import planar.cli.version;
export import planar.cli.output;
export import planar.cli.exit;

/// @file catalog.cpp
/// @brief Implementation of `planar.cmd.planar_execute.catalog`.

module planar.cmd.planar_execute.catalog;

import std;
import cli11;
import planar.cliapp.schema;

namespace planar::cmd::execute {

namespace {

/// @brief Declare one optional string flag with a description.
auto add_string(CLI::App& app, std::string_view name, std::string_view desc) -> void {
  app.add_option(std::string{name})->description(std::string{desc});
}

} // namespace

auto catalog_json() -> std::string {
  // Every string below is the catalog's own prose: `usage_text()` stays the
  // byte-pinned banner and is not derived from this tree, nor this tree from
  // it. The two are held together by the test that names each flag in both.
  CLI::App root{"Deterministic, spawn-free Lua workflow engine: runs one phase of a "
                "workflow over the allowlisted host surface (cli/git/fs/flow/ctx) and "
                "prints its flow.result payload as JSON on stdout. Holds no SQLite handle.",
                "planar-execute"};

  CLI::App* run = root.add_subcommand("run", "Load <workflow.lua> in the sandbox, register the deterministic host "
                                             "surface, call the named phase, and print the workflow's "
                                             "flow.result(table) payload as JSON on stdout. Usage failures exit 2; "
                                             "an unreadable workflow exits 1; the phase's flow.fail exits 1.");
  run->add_option("workflow")->required()->description("Path to the workflow file.");
  run->add_option("--phase")->required()->description("Phase function to invoke inside the workflow.");
  add_string(*run, "--args", "JSON args blob exposed to the phase as ctx.args.");
  add_string(*run, "--worktree", "Worktree directory the git/fs host functions are confined to.");
  add_string(*run, "--sandbox-root", "Sandbox root that bounds every fs path the workflow may touch.");
  add_string(*run, "--engine",
             "Execution engine: embedded or centurion. Overrides $PLANAR_EXECUTE_ENGINE and the execute.engine "
             "config key; default embedded. centurion is refused at dispatch until the Centurion host lands.");

  CLI::App* profile = root.add_subcommand("profile", "Inspect the resolved execution profile.");
  CLI::App* show    = profile->add_subcommand("show", "Print the resolved engine and the provenance that chose it "
                                                      "(flag, env, config file, or embedded default) on stdout.");
  add_string(*show, "--profile", "Execution profile to resolve ([execute.profiles.<name>]); default: default.");
  show->add_flag("--json")->description("Emit {\"engine\":…,\"engine_source\":…} instead of key: value lines.");

  root.add_subcommand("schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals).");

  static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> k_summaries{{
      {"planar-execute run", "Run one phase of a Lua workflow and print its flow.result JSON."},
      {"planar-execute profile", "Inspect the resolved execution profile."},
      {"planar-execute profile show", "Print the resolved engine and where it came from."},
      {"planar-execute schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals)."},
  }};
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 4> k_empty_defaults{{
      {"planar-execute run", "--args"},
      {"planar-execute run", "--worktree"},
      {"planar-execute run", "--sandbox-root"},
      {"planar-execute run", "--engine"},
  }};
  return cliapp::schema_json(root, k_summaries, k_empty_defaults);
}

} // namespace planar::cmd::execute

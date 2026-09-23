/// @file init.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.init`.

module planar.cmd.planar.handlers.init;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.db.migrate;
import planar.git;
import planar.engine.config.init;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace cfg = engine::config;
namespace git = planar::git;

namespace {

using kind_t = domain_error_kind;

/// @brief The `init_error` name the oracle's `@errorName` would print.
/// @param err The engine failure.
/// @return The Zig-spelled error name.
auto error_name(cfg::init_error err) -> std::string_view {
  switch (err) {
  case cfg::init_error::invalid_path:
    return "InvalidPath";
  case cfg::init_error::query_failed:
    return "QueryFailed";
  }
  return "QueryFailed";
}

} // namespace

auto probe_git_origin(const std::filesystem::path& dir) -> std::optional<std::string> {
  // Delegates to the layer-1 seam (plan 996, task 6128). This function used
  // to carry its own `popen` + `shell_quote` + trim + exit-status copy;
  // `planar.git` now owns all four, and `planar-agent/locality.cpp` had a
  // second copy of the same code. It stays exported under this name because
  // it is the oracle-named probe `init` runs and `init.t.cpp` tests every
  // one of its negative arms directly.
  //
  // Empty output is NOT an empty remote: the oracle rejects it after the
  // exit-status check, so `git` answering with a blank line stores SQL NULL.
  // `run_trimmed` is where that rule now lives.
  return git::probe_origin_url(dir);
}

auto init(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // Step 1: open + create the parent directory + migrate + schema-version
  // guard. All four live in `ensure_db`, which is where the Zig runtime's
  // `ensureDb` puts them too.
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // Read the applied version back for the report. The oracle swallows a
  // failure here and prints 0 rather than refusing — `intQuery(...) catch
  // 0` — so a database that migrated but whose version cannot be re-read
  // still reports success.
  auto const       version = db::current_version(**conn);
  cfg::init_result result{.db = ctx.db_path().string(), .schema_version = version.value_or(0)};
  bool const       json         = flag_bool(args, "--json");
  bool const       skip_project = flag_bool(args, "--skip-project");

  if (!skip_project) {
    // Step 2: register the working directory as a project.
    //
    // `ctx.cwd()` is PWD-first (see `planar.cmd.planar.context`), matching
    // the oracle's `scope_mod.operatorCwd`: preserving the shell's
    // spelling is what keeps a `/var/...` project matching the
    // `projects.root_path` key `assoc add` wrote on this platform, where
    // canonicalising would rewrite it to `/private/var/...`.
    auto const cwd = ctx.cwd().string();

    // Best-effort, handler-layer IO. See this module's header for why the
    // negative arms are the load-bearing ones.
    auto const remote = probe_git_origin(ctx.cwd());

    auto const name = flag_string(args, "--name");
    auto const slug = flag_string(args, "--slug");

    auto registered = cfg::register_cwd(**conn, cfg::register_cwd_args{
                                                    .cwd        = cwd,
                                                    .name       = name,
                                                    .slug       = slug,
                                                    .git_remote = remote,
                                                    .force      = flag_bool(args, "--force"),
                                                });
    if (!registered) {
      return std::unexpected(
          error_from_body(kind_t::generic_failure, std::format("registering project: {}", error_name(registered.error()))));
    }

    result.project_id   = registered->id;
    result.project_slug = registered->slug;
    result.project_name = registered->name;
    result.root_path    = registered->root_path;
    // Read back from the ROW, not from the probe: on a second, non-forced
    // `init` the pre-existing row's remote is what `INSERT OR IGNORE`
    // preserved, and that — not what git answered this time — is what the
    // oracle reports.
    result.git_remote = registered->git_remote;
  }

  ctx.out() << (json ? cfg::render_init_json(result) : cfg::render_init_text(result));
  return {};
}

/// @brief Declare the `init` leaf.
///
/// A LEAF, not a group. `--json` carries a DESCRIPTION here, unlike
/// the ninety-odd bare `--json` flags elsewhere in the tree, so it is
/// `add_bool` rather than `add_json` — `report` is the only other
/// such site.
auto declare_init(CLI::App& root) -> void {
  CLI::App* init = root.add_subcommand("init", "Initialize the Planar database and register the current directory as a project.");
  add_string(*init, "--name", "Project name (defaults to repo dir)");
  add_string(*init, "--slug",
             "Explicit project slug (defaults to a slug derived from the directory name); with --force, targets that "
             "registration for repoint");
  add_bool(*init, "--skip-project", "Only init the DB; skip project registration");
  add_bool(*init, "--allow-no-repo", "Allow initialization outside a git repo");
  add_bool(*init, "--force", "Overwrite an existing project registration");
  add_bool(*init, "--json", "Emit machine-readable output");
}

} // namespace planar::cmd::handlers

/// @file catalog.cppm
/// @brief `planar.cmd.planar_execute.catalog` — the `planar-execute schema`
/// catalog (plan 1033 M0, task 6486, decision 1030 / D18).
///
/// ## A catalog WITHOUT a parser swap
///
/// `planar-execute` keeps its hand-rolled argument parser
/// (`planar.cmd.planar_execute.cli`): the usage-to-stderr, bare-invocation-
/// is-exit-2 contract is pinned byte for byte and there is no reason to
/// move it onto CLI11 to describe it. What this module adds is a
/// DESCRIPTION of that surface in the same flat JSON shape the other four
/// binaries emit from `planar.cliapp.schema`, so `cli_usage_lint` can police
/// the verbs authored surfaces reference against it (decision 1030 reverses
/// the plan-996 exemption recorded in CLAUDE.md: the exemption existed
/// because the Zig oracle had no catalog to compare against, and the oracle
/// is gone).
///
/// The `CLI::App` tree built here is therefore data, not the parser. It is
/// constructed, rendered, and discarded inside `catalog_json()`; nothing
/// ever calls `parse` on it. `cli.t.cpp` pins the two halves against each
/// other — every flag `parse_run_args` accepts appears in the catalog and
/// every catalog flag is accepted — so they cannot drift apart silently.
module;

export module planar.cmd.planar_execute.catalog;

import std;

namespace planar::cmd::execute {

/// @brief Render the `planar-execute` command tree as the flat JSON catalog.
///
/// The document is a single line with no trailing newline (the `schema`
/// verb appends one at the write site), `"root"` is `planar-execute`, and
/// the three optional `run` flags report `"default":""` because the parser
/// treats an absent flag as the empty string — the same declaration
/// `planar workflow run` makes for the flags it forwards here.
/// @return The catalog JSON.
export auto catalog_json() -> std::string;

} // namespace planar::cmd::execute

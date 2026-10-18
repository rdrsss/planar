/// @file docs.cppm
/// @brief `planar.cmd.planar_agent.docs` — the `planar-agent` binary's per-command examples and
/// exit codes, supplied as data beside its summary table.
///
/// `CLI::App` has no slot for either, so they live here and reach two
/// consumers: `planar.cliapp.schema::schema_json` emits them as each leaf's
/// `docs.examples` and `docs.exitCodes`, and `install_docs_footers` renders
/// them as the `Examples:` and `Exit codes:` sections of that leaf's
/// `--help`. One table feeds both, so the two cannot drift. Only leaf verbs
/// have rows. Each leaf lists the exit codes its handler can return: the
/// binary-wide meaning of a code is in `k_meanings`, and a verb whose code
/// means something narrower overrides it in `k_overrides`.
module;

export module planar.cmd.planar_agent.docs;

import std;
import planar.cliapp.schema;

namespace planar::cmd::agent {

/// @brief The examples and exit codes of every `planar-agent` leaf verb.
///
/// The returned table borrows static storage and is valid for the life of
/// the process.
/// @return The table.
export auto surface_docs() -> const cliapp::command_docs& {
  static constexpr std::pair<std::string_view, std::string_view> k_examples[] = {
      {"planar-agent pull", R"(planar-agent pull 7 --vendor claude --role coder)"},
      {"planar-agent pull", R"(planar-agent pull 7 --vendor claude --role coder --ttl 4h --worktree ../wt)"},
      {"planar-agent peek", R"(planar-agent peek 7)"},
      {"planar-agent peek", R"(planar-agent peek 7 --json)"},
      {"planar-agent brief", R"(planar-agent brief 7)"},
      {"planar-agent brief", R"(planar-agent brief 7 --json)"},
      {"planar-agent complete",
       R"(planar-agent complete --claim 68fba76b319bcba1772f6e40a9b76f75 --summary "Landed the migration")"},
      {"planar-agent fail",
       R"(planar-agent fail --claim 68fba76b319bcba1772f6e40a9b76f75 --reason "Tests fail on the roundtrip")"},
      {"planar-agent release", R"(planar-agent release --claim 68fba76b319bcba1772f6e40a9b76f75 --reason "Out of turn budget")"},
      {"planar-agent block",
       R"(planar-agent block --claim 68fba76b319bcba1772f6e40a9b76f75 --blocker 43 --reason "Needs the schema change")"},
      {"planar-agent claim", R"(planar-agent claim --entity task:42 --vendor claude --role coder)"},
      {"planar-agent claim", R"(planar-agent claim --entity task:42 --no-transition)"},
      {"planar-agent heartbeat", R"(planar-agent heartbeat --claim 68fba76b319bcba1772f6e40a9b76f75)"},
      {"planar-agent heartbeat",
       R"(planar-agent heartbeat --claim 68fba76b319bcba1772f6e40a9b76f75 --status "validating: unit tests")"},
      {"planar-agent action start",
       R"(planar-agent action start --claim 68fba76b319bcba1772f6e40a9b76f75 --kind edit --entity task:42)"},
      {"planar-agent action end", R"(planar-agent action end --action 17 --outcome ok --summary "Edited the handler")"},
      {"planar-agent review",
       R"(planar-agent review --claim 68fba76b319bcba1772f6e40a9b76f75 --verdict approve --summary "Matches the spec" --reviewer-claim 0f3a9c1e5b7d2468a1c3e5f70b9d4a62)"},
      {"planar-agent review",
       R"(planar-agent review --claim 68fba76b319bcba1772f6e40a9b76f75 --verdict request-changes --summary "Missing a negative test" --vendor-session claude:reviewer-1)"},
      {"planar-agent ingest", R"(planar-agent ingest --vendor claude --event @event.json)"},
      {"planar-agent reconcile", R"(planar-agent reconcile --dry-run)"},
      {"planar-agent reconcile", R"(planar-agent reconcile --stale-after 2h)"},
      {"planar-agent abort",
       R"(planar-agent abort --claim 68fba76b319bcba1772f6e40a9b76f75 --reason "Operator stopped the session")"},
  };
  static constexpr std::pair<std::string_view, std::string_view> k_exit_codes[] = {
      {"planar-agent version", "0 1"},       {"planar-agent pull", "0 1 2 7"},         {"planar-agent peek", "0 1 2 7"},
      {"planar-agent brief", "0 1 2 7"},     {"planar-agent complete", "0 1 2 7"},     {"planar-agent fail", "0 1 2 7"},
      {"planar-agent release", "0 1 2 7"},   {"planar-agent block", "0 1 2 7"},        {"planar-agent claim", "0 1 2 7"},
      {"planar-agent heartbeat", "0 1 2 7"}, {"planar-agent action start", "0 1 2 7"}, {"planar-agent action end", "0 1 2 7"},
      {"planar-agent review", "0 1 2 7"},    {"planar-agent ingest", "0 1 2"},         {"planar-agent reconcile", "0 1 2 7"},
      {"planar-agent abort", "0 1 2 7"},     {"planar-agent schema", "0 1 2"},
  };
  static constexpr cliapp::exit_code_doc k_meanings[] = {
      {0, R"(Success.)"},
      {1, R"(Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag.)"},
      {2, R"(Bad input: an invalid value or entity ref.)"},
      {6, R"(Precondition conflict: the entity already exists or has already ended.)"},
      {7, R"(The database schema is behind or ahead of this binary.)"},
  };
  static constexpr cliapp::exit_meaning_override k_overrides[] = {
      {"planar-agent brief", 0,
       R"(The brief was written, including when the config file is invalid (policy.valid is then false). Nothing is written to the database.)"},
      {"planar-agent brief", 1, R"(No plan has the id, or the database could not be opened; or a usage error.)"},
      {"planar-agent brief", 2, R"(The plan id is not a positive integer.)"},
      {"planar-agent review", 1,
       R"(No such claim, or the review came from the reviewed claim's own session (nothing is written); or a usage error.)"},
      {"planar-agent review", 2,
       R"(Missing or invalid --verdict, missing --summary, or neither --reviewer-claim nor --vendor-session was given.)"},
  };
  static constexpr cliapp::command_docs k_docs{k_examples, k_exit_codes, k_meanings, k_overrides};
  return k_docs;
}

} // namespace planar::cmd::agent

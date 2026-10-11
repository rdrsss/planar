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
      {"planar-agent claim-associate",
       R"(planar-agent claim-associate --claim 68fba76b319bcba1772f6e40a9b76f75 --run 3 --stage build)"},
      {"planar-agent action start",
       R"(planar-agent action start --claim 68fba76b319bcba1772f6e40a9b76f75 --kind edit --entity task:42)"},
      {"planar-agent action end", R"(planar-agent action end --action 17 --outcome ok --summary "Edited the handler")"},
      {"planar-agent ingest", R"(planar-agent ingest --vendor claude --event @event.json)"},
      {"planar-agent reconcile", R"(planar-agent reconcile --dry-run)"},
      {"planar-agent reconcile", R"(planar-agent reconcile --stale-after 2h)"},
      {"planar-agent abort",
       R"(planar-agent abort --claim 68fba76b319bcba1772f6e40a9b76f75 --reason "Operator stopped the session")"},
      {"planar-agent run start",
       R"(planar-agent run start --plan 7 --workflow build --run-id run-1a2b3c --repo-root . --ttl 10m)"},
      {"planar-agent run end", R"(planar-agent run end --run-id run-1a2b3c --status completed)"},
      {"planar-agent run heartbeat", R"(planar-agent run heartbeat --run-id run-1a2b3c --ttl 10m)"},
      {"planar-agent dispatch preview",
       R"(planar-agent dispatch preview --work-item 42 --project 1 --validation-policy default --routing-policy default --profile-rule rule-1 --vendor claude --role coder --tier medium --work-type feature --complexity standard --packet-digest sha256:aa --profile-digest sha256:bb --policy-digest sha256:cc --capability-digest sha256:dd --candidate 3 --host devbox --class default --evidence-state evidential --expires-at 2026-10-05T09:00:00Z)"},
      {"planar-agent dispatch confirm",
       R"(planar-agent dispatch confirm --token tok-1 --dispatch-key key-1 --now 2026-10-04T09:00:00Z --packet-digest sha256:aa --profile-digest sha256:bb --policy-digest sha256:cc --capability-digest sha256:dd --candidate 3 --vendor claude --role coder --tier medium --work-type feature --complexity standard --validation-policy default --routing-policy default)"},
      {"planar-agent context add",
       R"(planar-agent context add --claim 68fba76b319bcba1772f6e40a9b76f75 --kind finding --body "Backoff cap is 30 seconds")"},
      {"planar-agent context capsule", R"(planar-agent context capsule --run 3 --stage build --body "Build stage notes")"},
      {"planar-agent context list", R"(planar-agent context list --run 3)"},
      {"planar-agent context list", R"(planar-agent context list --run 3 --status open --json)"},
      {"planar-agent context resolve", R"(planar-agent context resolve --id 5 --status consumed)"},
      {"planar-agent queue run", R"(planar-agent queue run -- make test)"},
      {"planar-agent queue run", R"(planar-agent queue run --detach --vendor claude --role coder -- make test)"},
      {"planar-agent queue cancel", R"(planar-agent queue cancel 1000001)"},
      {"planar-agent queue status", R"(planar-agent queue status 1000001)"},
      {"planar-agent queue status", R"(planar-agent queue status 1000001 --json)"},
      {"planar-agent queue wait", R"(planar-agent queue wait 1000001 --timeout 3h --json)"},
      {"planar-agent queue rule", R"(planar-agent queue rule)"},
  };
  static constexpr std::pair<std::string_view, std::string_view> k_exit_codes[] = {
      {"planar-agent version", "0 1"},
      {"planar-agent pull", "0 1 2 7"},
      {"planar-agent peek", "0 1 2 7"},
      {"planar-agent complete", "0 1 2 7"},
      {"planar-agent fail", "0 1 2 7"},
      {"planar-agent release", "0 1 2 7"},
      {"planar-agent block", "0 1 2 7"},
      {"planar-agent claim", "0 1 2 7"},
      {"planar-agent heartbeat", "0 1 2 7"},
      {"planar-agent claim-associate", "0 1 2 7"},
      {"planar-agent action start", "0 1 2 7"},
      {"planar-agent action end", "0 1 2 7"},
      {"planar-agent ingest", "0 1 2"},
      {"planar-agent reconcile", "0 1 2 7"},
      {"planar-agent abort", "0 1 2 7"},
      {"planar-agent schema", "0 1 2"},
      {"planar-agent run start", "0 1 2 7"},
      {"planar-agent run end", "0 1 2 7"},
      {"planar-agent run heartbeat", "0 1 2 7"},
      {"planar-agent dispatch preview", "0 1 2 7"},
      {"planar-agent dispatch confirm", "0 1 2 7"},
      {"planar-agent context add", "0 1 2 7"},
      {"planar-agent context capsule", "0 1 2 7"},
      {"planar-agent context list", "0 1 2 7"},
      {"planar-agent context resolve", "0 1 2 7"},
      {"planar-agent queue run", "0 1 2 124 125 126 127"},
      {"planar-agent queue cancel", "0 1 2 6 125"},
      {"planar-agent queue status", "0 1 2 125"},
      {"planar-agent queue wait", "0 1 2 124 125 126 127 130 143"},
      {"planar-agent queue rule", "0 1"},
  };
  static constexpr cliapp::exit_code_doc k_meanings[] = {
      {0, R"(Success.)"},
      {1, R"(Failure: entity or claim not found, an unmapped error, or a usage error such as an unknown flag.)"},
      {2, R"(Bad input: an invalid value or entity ref.)"},
      {6, R"(Precondition conflict: the entity already exists or has already ended.)"},
      {7, R"(The database schema is behind or ahead of this binary.)"},
      {124, R"(The command was stopped at its run limit (--timeout).)"},
      {125,
       R"(The queue failed: planar.db is unreachable or incompatible, the wait limit passed, the entry was cancelled, or an internal error.)"},
      {126, R"(The command was found but could not be executed.)"},
      {127, R"(The command was not found.)"},
      {130, R"(Observation was interrupted by SIGINT.)"},
      {143, R"(Observation was interrupted by SIGTERM.)"},
  };
  static constexpr cliapp::exit_meaning_override k_overrides[] = {
      {"planar-agent queue run", 0,
       R"(The command exited 0. Any other status of the command passes through unchanged, as 128 plus N when signal N ended it.)"},
      {"planar-agent queue run", 2, R"(Refused before queueing: the command is a model launcher or --timeout is invalid.)"},
      {"planar-agent queue run", 1, R"(A usage error; the command was not run.)"},
      {"planar-agent queue cancel", 6, R"(The entry has already ended.)"},
      {"planar-agent queue cancel", 1, R"(No such entry, or a usage error.)"},
      {"planar-agent queue status", 1, R"(No such entry, or a usage error.)"},
      {"planar-agent queue wait", 1, R"(Ticket history is unavailable, or a usage error.)"},
      {"planar-agent queue wait", 2, R"(Invalid sequence or observation timeout.)"},
      {"planar-agent queue wait", 124,
       R"(Observer deadline expired, or recorded command timeout or exit 124; inspect wait_reason and status.)"},
      {"planar-agent queue wait", 125,
       R"(Observation failed or stalled, or recorded command exit 125, cancellation or wait timeout; inspect wait_reason and status.)"},
      {"planar-agent queue rule", 1, R"(A usage error.)"},
  };
  static constexpr cliapp::command_docs k_docs{k_examples, k_exit_codes, k_meanings, k_overrides};
  return k_docs;
}

} // namespace planar::cmd::agent

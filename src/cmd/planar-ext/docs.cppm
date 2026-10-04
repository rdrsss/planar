/// @file docs.cppm
/// @brief `planar.cmd.planar_ext.docs` — the `planar-ext` binary's per-command examples and
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

export module planar.cmd.planar_ext.docs;

import std;
import planar.cliapp.schema;

namespace planar::cmd::ext {

/// @brief The examples and exit codes of every `planar-ext` leaf verb.
///
/// The returned table borrows static storage and is valid for the life of
/// the process.
/// @return The table.
export auto surface_docs() -> const cliapp::command_docs& {
  static constexpr std::pair<std::string_view, std::string_view> k_examples[] = {
      {"planar-ext ext register jira",
       R"(planar-ext ext register jira jira-main --base-url https://example.atlassian.net --project PLAN --auth-env JIRA_TOKEN)"},
      {"planar-ext ext register github",
       R"(planar-ext ext register github github-main --project acme/planar --auth-env GITHUB_TOKEN)"},
      {"planar-ext ext list", R"(planar-ext ext list --json)"},
      {"planar-ext ext test", R"(planar-ext ext test github-main)"},
      {"planar-ext ext create", R"(planar-ext ext create github-main --from task:42)"},
      {"planar-ext ext propagate-one", R"(planar-ext ext propagate-one github-main --from task:42 --dry-run)"},
      {"planar-ext ext propagate", R"(planar-ext ext propagate 7 --system github-main --dry-run)"},
      {"planar-ext ext propagate", R"(planar-ext ext propagate 7 --system github-main --yes)"},
      {"planar-ext sync pull", R"(planar-ext sync pull task:42)"},
      {"planar-ext sync pull", R"(planar-ext sync pull --all --system github-main)"},
      {"planar-ext sync push", R"(planar-ext sync push task:42)"},
      {"planar-ext sync push", R"(planar-ext sync push --all --system github-main)"},
      {"planar-ext sync status", R"(planar-ext sync status --system github-main --json)"},
      {"planar-ext sync resolve",
       R"(planar-ext sync resolve 5 --keep local --evidence-token tok-1 --expected-local-updated-at 2026-10-04T09:00:00Z)"},
  };
  static constexpr std::pair<std::string_view, std::string_view> k_exit_codes[] = {
      {"planar-ext version", "0 1"},
      {"planar-ext schema", "0 1 2"},
      {"planar-ext ext register jira", "0 1 2 6 7"},
      {"planar-ext ext register github", "0 1 2 6 7"},
      {"planar-ext ext list", "0 1 7"},
      {"planar-ext ext test", "0 1 2 7"},
      {"planar-ext ext create", "0 1 2 6 7"},
      {"planar-ext ext propagate-one", "0 1 2 7"},
      {"planar-ext ext propagate", "0 1 2 7"},
      {"planar-ext sync pull", "0 1 2 3 5 7"},
      {"planar-ext sync push", "0 1 2 3 5 7"},
      {"planar-ext sync status", "0 1 2 7"},
      {"planar-ext sync resolve", "0 1 2 3 5 7"},
  };
  static constexpr cliapp::exit_code_doc k_meanings[] = {
      {0, R"(Success.)"},
      {1, R"(Failure: entity not found, an unmapped error, or a usage error such as an unknown flag.)"},
      {2, R"(Bad input: an invalid value.)"},
      {3, R"(Operational-plane sync conflict; resolve it with sync resolve.)"},
      {5, R"(Cross-scope write refused: the entity belongs to a scope the resolved scope does not cover.)"},
      {6, R"(Precondition conflict: the slug is taken or the link already exists.)"},
      {7, R"(The database schema is behind or ahead of this binary.)"},
  };
  static constexpr cliapp::command_docs k_docs{k_examples, k_exit_codes, k_meanings, {}};
  return k_docs;
}

} // namespace planar::cmd::ext

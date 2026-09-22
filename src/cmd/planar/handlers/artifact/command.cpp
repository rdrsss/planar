/// @file artifact.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.artifact`.

module planar.cmd.planar.handlers.artifact;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.planning;
import planar.engine.entitylink;
import planar.engine.runtime;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.handlers.links;
import planar.cmd.planar.scope;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.artifact.add;
import planar.cmd.planar.handlers.artifact.show;
import planar.cmd.planar.handlers.artifact.list;
import planar.cmd.planar.handlers.artifact.update;
import planar.cmd.planar.handlers.artifact.edit;
import planar.cmd.planar.handlers.artifact.view;
import planar.cmd.planar.handlers.artifact.diff;
import planar.cmd.planar.handlers.artifact.review;
import planar.cmd.planar.handlers.artifact.link;

namespace planar::cmd::handlers {

namespace pl       = engine::planning;
namespace session  = engine::runtime::session;
namespace activity = engine::runtime::agentactivity;

namespace {

/// @brief The vendor identity for the session the create hook attaches to.
///
/// Unset AND empty both yield `cli` — the Zig original's semantics, not
/// "unset -> default". Same discipline as `handlers/question.cpp`'s copy.
/// @param ctx The invocation context.
/// @return The vendor identity.
auto vendor_from(const context& ctx) -> std::string {
  auto const value = ctx.env()("PLANAR_VENDOR");
  if (!value.has_value() || value->empty()) {
    return "cli";
  }
  return *value;
}

/// @brief The vendor's own session id, when it published one.
/// @param ctx The invocation context.
/// @return The vendor session id, or unset.
auto vendor_session_id_from(const context& ctx) -> std::optional<std::string> {
  auto const value = ctx.env()("PLANAR_VENDOR_SESSION_ID");
  if (!value.has_value() || value->empty()) {
    return std::nullopt;
  }
  return value;
}

/// @brief The Zig error name for an `artifact_error`.
///
/// zig's handlers die with `exit.die(ctx, e, "artifact add: {s}",
/// .{@errorName(e)})`, so the operator-visible message carries Zig's
/// CamelCase error TAG. `SlugNotFound`, `NotFound` and
/// `IllegalTransition` were captured from the oracle directly (`artifact
/// list --scope nosuch`, `artifact add --plan 999`, `artifact update` on a
/// draft row targeting `retired`).
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(pl::artifact_error err) -> std::string_view {
  switch (err) {
  case pl::artifact_error::not_found:
    return "NotFound";
  case pl::artifact_error::unsupported_scope:
    return "UnsupportedScope";
  case pl::artifact_error::slug_not_found:
    return "SlugNotFound";
  case pl::artifact_error::illegal_transition:
    return "IllegalTransition";
  case pl::artifact_error::no_fields:
    return "NoFields";
  case pl::artifact_error::query_failed:
    return "QueryFailed";
  case pl::artifact_error::audit_write_failed:
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Map an `artifact_error` onto this binary's exit-code bucket.
///
/// EVERY member lands in the generic bucket (exit 1) — including
/// `IllegalTransition`, verified by running `artifact update` across all
/// sixteen status edges against the oracle. The refusals that DO exit 2
/// (`unknown kind '<tok>'`, `unknown status '<tok>'`, the `--body` /
/// `--from-file` pair) come from this handler's own `invalid_input`, never
/// from here.
/// @param err The engine error.
/// @param verb The verb name to lead the message with, e.g. `"artifact add"`.
/// @return The mapped failure.
auto map_artifact_error(pl::artifact_error err, std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::generic_failure, std::format("{}: {}", verb, zig_error_name(err)));
}

/// @brief Map an engine error for a single-id verb, giving `not_found` the
/// oracle's own dedicated prose instead of the generic tag shape.
///
/// This family uses BOTH formats and the split is by verb shape, not by
/// error: `artifact show 999` is prose, `artifact list --scope nosuch` is
/// the tag. Both captured.
/// @param err The engine error.
/// @param verb The verb name to lead a generic message with.
/// @param id The artifact id, interpolated into the `not_found` message.
/// @return The mapped failure.
auto map_lookup_error(pl::artifact_error err, std::string_view verb, std::int64_t id) -> domain_error {
  if (err == pl::artifact_error::not_found) {
    // ORACLE: `error: no artifact with id 999`, from `show` and `update`
    // alike.
    return error_from_body(domain_error_kind::not_found, std::format("no artifact with id {}", id));
  }
  return map_artifact_error(err, verb);
}

/// @brief Split a comma-separated flag value into trimmed, non-empty tokens.
///
/// A fifth copy of the rule `handlers/plan.cpp`, `task.cpp`,
/// `question.cpp` and `scenario.cpp` each carry, kept local for the same
/// reason they are: the units are separate and this is vocabulary, not
/// policy.
///
/// The EMPTY-token skip is load-bearing, and its CONSEQUENCE differs per
/// flag. `--kind ""` yields no tokens and so applies no kind predicate.
/// `--status ""` also yields no tokens, but an empty status set is the
/// engine's `{draft, active}` default arm — so it behaves like omitting
/// the flag, not like "every status". Neither is an error and neither
/// lists empty; only a fixture holding `superseded`/`retired` rows can
/// tell the two apart, which is how it was settled.
/// @param raw The flag's raw value.
/// @return The non-empty, trimmed tokens, in order.
auto split_csv(std::string_view raw) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const part : std::views::split(raw, ',')) {
    std::string_view tok{part.begin(), part.end()};
    while (!tok.empty() && tok.front() == ' ') {
      tok.remove_prefix(1);
    }
    while (!tok.empty() && tok.back() == ' ') {
      tok.remove_suffix(1);
    }
    if (!tok.empty()) {
      out.emplace_back(tok);
    }
  }
  return out;
}

/// @brief Resolve `--body`'s `@path` grammar, mapping a read failure onto
/// the oracle's refusal.
///
/// ORACLE: `error: read --body: FileNotFound` at exit 2 — note it does NOT
/// name the path, where the `--from-file` refusal does. Both captured; the
/// asymmetry is the oracle's.
/// @param raw The raw `--body` value.
/// @return The body bytes, or the refusal.
auto body_from_flag(std::string_view raw) -> std::expected<std::string, domain_error> {
  auto read = pl::read_body(raw);
  if (!read) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "read --body: FileNotFound"));
  }
  return *read;
}

/// @brief Write one artifact through the requested renderer.
///
/// Per-renderer terminator contract: `render_text` carries its own
/// trailing newline, `render_json` is a fragment this caller terminates.
/// @param ctx The invocation context.
/// @param args The parsed arguments (read for `--json`).
/// @param a The artifact to render.
void emit(context& ctx, const cliapp::parsed_args& args, const pl::artifact& a) {
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_json(a) << '\n';
  } else {
    ctx.out() << pl::render_text(a);
  }
}

/// @brief Parse a `--kind` / `--status` token set, refusing at exit 2 on
/// the first unknown token.
///
/// Both flags refuse with `unknown <noun> '<tok>'` and BOTH exit 2. The
/// character-identical refusal on `scenario list --status` exits 1,
/// because that one dies with a tag `codeFor` has no arm for. One flag
/// name, two families, two codes — captured on both, not reasoned about.
/// @tparam T The enum being parsed.
/// @param raw The raw flag value.
/// @param parse The token parser.
/// @param noun The noun for the refusal, `"kind"` or `"status"`.
/// @return The parsed values in order, or the refusal.
template <typename T>
auto parse_token_set(std::string_view raw, auto parse, std::string_view noun) -> std::expected<std::vector<T>, domain_error> {
  std::vector<T> out;
  for (auto const& tok : split_csv(raw)) {
    auto const parsed = parse(tok);
    if (!parsed) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown {} '{}'", noun, tok)));
    }
    out.push_back(*parsed);
  }
  return out;
}

} // namespace

auto artifact_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before any argument validation — zig's handler opens with
  // `try runtime.ensureDb()`, so even a refused invocation leaves a
  // created-and-migrated database behind.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const scope_flag = cliapp::flag_string(args, "--scope");
  auto const scope_view =
      scope_flag.has_value() ? std::optional<std::string_view>{*scope_flag} : std::optional<std::string_view>{};
  auto resolved = resolve_write_scope(ctx, scope_view, "artifact add");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }
  // NO `project_unassociated` refusal — `resolved->scope` staying unset
  // inside an unassociated project is the CORRECT outcome and lets the
  // engine write `scope_kind='global'`. Captured both ways in one arena,
  // before and after `assoc add`.

  // `--editor` is DECLARED with `default_value = "true"` and the oracle
  // prints NOTHING for it — no warning, no editor. `scenario add --editor`
  // writes a `warning: --editor not yet implemented` line to stderr;
  // reproducing that here would put a line on stderr the oracle never
  // writes. Deliberately unhandled.

  auto title = cliapp::positional_string(args, "title");
  if (!title) {
    // Unreachable through the CLI11 tree (the positional is declared
    // required), but an absent title must never fall through to "" and
    // write an untitled row.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "artifact add: title is required"));
  }

  auto const kind_raw = cliapp::flag_string(args, "--kind");
  if (!kind_raw.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "artifact add: --kind is required"));
  }
  auto const kind = pl::artifact_kind_from_text(*kind_raw);
  if (!kind) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown kind '{}'", *kind_raw)));
  }

  // `--status` DEFAULTS to `draft` here, which is NOT the column default
  // (`active`). The surface declares the default and the engine binds
  // whatever it is given, so a bare `artifact add` lands `draft`.
  auto const status_raw  = cliapp::flag_string(args, "--status");
  auto const status_text = status_raw.has_value() ? std::string_view{*status_raw} : std::string_view{"draft"};
  auto const status      = pl::artifact_status_from_text(status_text);
  if (!status) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown status '{}'", status_text)));
  }

  auto const body_flag = cliapp::flag_string(args, "--body");
  auto const from_file = cliapp::flag_string(args, "--from-file");
  if (body_flag.has_value() && from_file.has_value()) {
    // ORACLE, verbatim, exit 2.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--body and --from-file are mutually exclusive"));
  }

  std::optional<std::string> body;
  std::optional<std::string> source_path = cliapp::flag_string(args, "--source-path");
  if (body_flag.has_value()) {
    auto read = body_from_flag(*body_flag);
    if (!read) {
      return std::unexpected(read.error());
    }
    body = std::move(*read);
  } else if (from_file.has_value()) {
    // `--from-file` fills BOTH columns: the file's bytes become the body
    // AND the flag's value becomes `source_path` — stored as GIVEN, not
    // absolutized. Captured: `--from-file fm.md` lands
    // `"source_path":"fm.md"`. An explicit `--source-path` alongside it
    // still wins, because it is read above and only defaulted here.
    //
    // The read is RAW. A file opening with a `---` front-matter block
    // reaches the column WITH that block; nothing on this path strips it.
    auto read = pl::read_body(std::format("@{}", *from_file));
    if (!read) {
      // ORACLE names the path here, where the `--body` refusal does not.
      return std::unexpected(
          error_from_body(domain_error_kind::invalid_input, std::format("read --from-file {}: FileNotFound", *from_file)));
    }
    body = std::move(*read);
    if (!source_path.has_value()) {
      source_path = *from_file;
    }
  }

  // The session is resolved AFTER every argument refusal and BEFORE the
  // create, and its creation is a COMMITTED SIDE EFFECT even when the
  // create then fails. The ordering was pinned by running each refusal
  // against a FRESH scratch root and counting `sessions`:
  //   `--kind nosuch`     -> exit 2, sessions = 0
  //   `--body @missing`   -> exit 2, sessions = 0
  //   `--scope nosuchslug`-> exit 1, sessions = 1
  // So the argument refusals land ahead of it and the engine's own
  // scope refusal lands behind it. Moving this call earlier would leave a
  // `sessions` row behind for two refusals that write none.
  //
  // `artifact add` is the ONLY leaf in this family that touches
  // `sessions`, and `scenario add` — its closest sibling — starts none at
  // all. Captured, not inherited.
  auto const session_id = session::ensure_active(**conn, vendor_from(ctx), vendor_session_id_from(ctx));

  auto created = pl::create_artifact(**conn, pl::artifact_create_args{
                                                 .title       = *title,
                                                 .kind        = *kind,
                                                 .body        = std::move(body),
                                                 .source_path = std::move(source_path),
                                                 .status      = *status,
                                                 .plan_id     = cliapp::flag_int(args, "--plan"),
                                                 .scope       = resolved->scope,
                                             });
  if (!created) {
    // A nonexistent `--plan` arrives here as `not_found` and renders
    // `artifact add: NotFound` — the TAG shape, not the `no artifact with
    // id N` prose, because the missing entity is the plan and not the
    // artifact. Oracle-captured.
    return std::unexpected(map_artifact_error(created.error(), "artifact add"));
  }

  // The entity-create activity hook, composed HERE because the engine
  // cannot reach `engine_runtime` from layer 2. Runs after the create has
  // committed and swallows every failure, so it can only add an
  // `agent_actions` row, never change this verb's outcome. A session with
  // no live claim makes it a silent no-op — the interactive-operator case.
  //
  // The summary carries the KIND as well as the title, matching the
  // engine's own audit summary and unlike `question`'s, which is the title
  // alone.
  if (session_id) {
    activity::record_entity_create_action(
        **conn, *session_id, activity::action_entity_kind::artifact, created->id,
        std::format("created artifact: {} (kind={})", *title, pl::artifact_kind_to_text(*kind)));
  }

  emit(ctx, args, *created);
  return {};
}

auto artifact_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "artifact-id", "artifact");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto found = pl::show_artifact(**conn, *id);
  if (!found) {
    return std::unexpected(map_lookup_error(found.error(), "artifact show", *id));
  }
  emit(ctx, args, *found);
  return {};
}

auto artifact_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  pl::artifact_list_filter filter{};
  filter.plan_id = cliapp::flag_int(args, "--plan");

  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    auto parsed = parse_token_set<pl::artifact_status>(*raw, pl::artifact_status_from_text, "status");
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    // An EMPTY result — no tokens, from `--status ""` — leaves
    // `filter.statuses` empty, which the engine reads as `{draft, active}`
    // and NOT as "every status". That is the family's own default and the
    // single most copy-prone value across the four planning families.
    filter.statuses = std::move(*parsed);
  }

  if (auto const raw = cliapp::flag_string(args, "--kind"); raw.has_value()) {
    auto parsed = parse_token_set<pl::artifact_kind>(*raw, pl::artifact_kind_from_text, "kind");
    if (!parsed) {
      return std::unexpected(parsed.error());
    }
    filter.kinds = std::move(*parsed);
  }

  // Note the target: `filter.scopes`, never `filter.scope`. An explicit
  // `--scope` REPLACES the cwd read set and is comma-split into the
  // vector; the engine resolves each slug and reports its own
  // `SlugNotFound` for any member that does not resolve.
  if (auto const raw = cliapp::flag_string(args, "--scope"); raw.has_value()) {
    filter.scopes = split_csv(*raw);
  } else {
    auto slugs = resolve_read_scope_slugs(ctx);
    if (!slugs) {
      return std::unexpected(slugs.error());
    }
    filter.scopes = std::move(*slugs);
  }

  auto rows = pl::list_artifacts(**conn, filter);
  if (!rows) {
    return std::unexpected(map_artifact_error(rows.error(), "artifact list"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << pl::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << pl::render_list_text(*rows);
  }
  return {};
}

auto artifact_update(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto const id = entity_id_arg(args, "artifact-id", "artifact");
  if (!id) {
    return std::unexpected(id.error());
  }

  pl::artifact_update_args patch{};
  patch.title       = cliapp::flag_string(args, "--title");
  patch.source_path = cliapp::flag_string(args, "--source-path");
  patch.scope       = cliapp::flag_string(args, "--scope");

  if (auto const raw = cliapp::flag_string(args, "--body"); raw.has_value()) {
    // Same `@path` grammar as `add`, same RAW read. `artifact update <id>
    // --body @<front-mattered-file>` stores the front matter too —
    // verified by round-tripping a `---`-prefixed file through the oracle
    // and reading the column back.
    auto read = body_from_flag(*raw);
    if (!read) {
      return std::unexpected(read.error());
    }
    patch.body = std::move(*read);
  }

  if (auto const raw = cliapp::flag_string(args, "--status"); raw.has_value()) {
    auto const st = pl::artifact_status_from_text(*raw);
    if (!st) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown status '{}'", *raw)));
    }
    patch.status = *st;
  }

  if (!patch.title.has_value() && !patch.body.has_value() && !patch.status.has_value() && !patch.source_path.has_value() &&
      !patch.scope.has_value()) {
    // ORACLE, verbatim, exit 1 — PROSE, not the `NoFields` tag the engine
    // would produce. The engine's own `no_fields` arm is therefore
    // unreachable through the CLI, and is kept only so its mapping is
    // total against zig's error set.
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, "at least one field must be specified for update"));
  }

  auto updated = pl::update_artifact(**conn, *id, patch);
  if (!updated) {
    return std::unexpected(map_lookup_error(updated.error(), "artifact update", *id));
  }
  emit(ctx, args, *updated);
  return {};
}

auto artifact_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // `false`: the ASCII `->`. Only `plan link` uses the unicode arrow.
  return entity_link_verb(ctx, args, engine::entitylink::entity_kind::artifact, "artifact-id", "artifact", "artifact_id",
                          "artifact link", false);
}

namespace {

/// @brief Declare every child of the `artifact` group, in catalog order.
/// @param artifact The `artifact` group node.
auto declare_artifact_children(CLI::App& artifact) -> void {
  artifact_cli::attach_add(artifact);

  artifact_cli::attach_show(artifact);

  artifact_cli::attach_list(artifact);

  artifact_cli::attach_update(artifact);

  artifact_cli::attach_edit(artifact);

  artifact_cli::attach_view(artifact);

  artifact_cli::attach_diff(artifact);

  artifact_cli::attach_review(artifact);

  artifact_cli::attach_link(artifact);
}

} // namespace

auto declare_artifact(CLI::App& root) -> void {
  CLI::App* artifact = root.add_subcommand(
      "artifact", "Manage artifacts — durable documents that crystallize from work.\n\n  Kinds: tech_spec, adr, design_note, "
                  "summary, readme, generated,\n  other, product_spec, roadmap, research, getting_started,\n  changelog_entry, "
                  "glossary_term.\n  Status lifecycle: draft → active → superseded/retired.");
  artifact->require_subcommand(0);
  declare_artifact_children(*artifact);
}

} // namespace planar::cmd::handlers

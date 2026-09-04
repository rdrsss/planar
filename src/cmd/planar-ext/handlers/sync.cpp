/// @file sync.cpp
/// @brief Implementation of `planar.cmd.planar_ext.handlers.sync`. See sync.cppm
/// for why these three leaves need nothing from `engine_extsync`, why the
/// oracle's `Handle`-kind switch does not survive the port, and why `pull`
/// has a conflict exit arm that `push` does not.

module planar.cmd.planar_ext.handlers.sync;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.adapter;
import planar.engine.identity;
import planar.engine.external;
import planar.cmd.planar_ext.context;
import planar.cmd.planar_ext.exit;
import planar.cmd.planar_ext.handler;
import planar.cmd.planar_ext.scope;
import planar.cmd.planar_ext.handlers.ext_adapter_factory;

namespace planar::cmd::ext::handlers {

namespace link_ns   = engine::external::link;
namespace sync_ns   = engine::external::sync;
namespace system_ns = engine::external::system;

namespace {

/// @brief The Zig `@errorName` spelling of a `sync_error`.
///
/// The oracle interpolates `@errorName(e)` into both the per-link text line
/// and the JSON `detail`, so these strings are OBSERVABLE CLI output, not
/// diagnostics. They are the enumerator names of
/// `zig/src/engine/external/sync.zig`'s `Error` set verbatim — notably
/// `ReadOnly`, which is the one an operator actually hits (pushing a
/// `read-only` link).
/// @param err The engine failure.
/// @return The Zig error name.
auto sync_error_name(sync_ns::sync_error err) -> std::string_view {
  switch (err) {
  case sync_ns::sync_error::not_found:
    return "NotFound";
  case sync_ns::sync_error::query_failed:
    return "QueryFailed";
  case sync_ns::sync_error::read_only:
    return "ReadOnly";
  case sync_ns::sync_error::unsupported_entity_kind:
    return "UnsupportedEntityKind";
  case sync_ns::sync_error::not_conflict:
    return "NotConflict";
  case sync_ns::sync_error::stale_conflict:
    return "StaleConflict";
  case sync_ns::sync_error::evidence_changed:
    return "EvidenceChanged";
  case sync_ns::sync_error::adapter_failed:
    return "AdapterFailed";
  }
  return "QueryFailed";
}

/// @brief The Zig `@errorName` spelling of a `factory_error`.
///
/// ## THE `sync` VERBS DO NOT RENDER AN ADAPTER-BUILD FAILURE THE WAY
/// ## `ext test` DOES, AND THE DIFFERENCE IS OBSERVABLE
///
/// `ext test` maps a factory refusal through `factory_error_message`, which
/// produces operator-facing prose ("token env var 'DEMO_TOKEN' is not set")
/// at exit 2. The three `sync` verbs do NOT: their oracle handlers call
/// `exit.die(ctx, e, "… build adapter …: {s}", .{@errorName(e)})`, so they
/// emit the RAW Zig error tag, and none of these tags is in the exit-code
/// map, so they land at exit **1**.
///
/// This was ported wrongly first — by copying `ext test`, the nearest wired
/// sibling — and the parity lane caught it: it does not set the token
/// variable, so every result-stream case took the factory-refusal path and
/// showed `2 == 1` against the oracle's `1`. Deriving a verb's behaviour
/// from a sibling rather than from the oracle is exactly the failure this
/// milestone keeps repeating.
/// @param err The factory refusal.
/// @return The Zig error name.
auto factory_error_name(factory_error err) -> std::string_view {
  switch (err) {
  case factory_error::unsupported_auth_method:
    return "UnsupportedAuthMethod";
  case factory_error::token_env_var_missing:
    return "TokenEnvVarMissing";
  case factory_error::gh_cli_not_found:
    return "GhCliNotFound";
  case factory_error::gh_cli_failed:
    return "GhCliFailed";
  case factory_error::gh_cli_empty_token:
    return "GhCliEmptyToken";
  case factory_error::unsupported_system_kind:
    return "UnsupportedSystemKind";
  }
  return "UnsupportedSystemKind";
}

/// @brief The Zig `@errorName` spelling of a `link_error`.
/// @param err The engine failure.
/// @return The Zig error name.
auto link_error_name(link_ns::link_error err) -> std::string_view {
  switch (err) {
  case link_ns::link_error::not_found:
    return "NotFound";
  case link_ns::link_error::link_exists:
    return "LinkExists";
  case link_ns::link_error::query_failed:
    return "QueryFailed";
  }
  return "QueryFailed";
}

/// @brief The Zig `@errorName` spelling of a `system_error`.
/// @param err The engine failure.
/// @return The Zig error name.
auto system_error_name(system_ns::system_error err) -> std::string_view {
  switch (err) {
  case system_ns::system_error::not_found:
    return "NotFound";
  case system_ns::system_error::slug_exists:
    return "SlugExists";
  case system_ns::system_error::query_failed:
    return "QueryFailed";
  }
  return "QueryFailed";
}

/// @brief The table a link's entity kind lives in, for the scope lookup.
///
/// `session` is absent deliberately — the caller returns before reaching
/// here, because a session carries no scope pair at all.
/// @param kind The entity kind.
/// @return The table name, or unset for `session`.
auto scope_table_for(link_ns::external_entity_kind kind) -> std::optional<std::string_view> {
  switch (kind) {
  case link_ns::external_entity_kind::plan:
    return "plans";
  case link_ns::external_entity_kind::task:
    return "tasks";
  case link_ns::external_entity_kind::question:
    return "questions";
  case link_ns::external_entity_kind::test_scenario:
    return "test_scenarios";
  case link_ns::external_entity_kind::artifact:
    return "artifacts";
  case link_ns::external_entity_kind::decision:
    return "decisions";
  case link_ns::external_entity_kind::session:
    return std::nullopt;
  }
  return std::nullopt;
}

/// @brief Decode a stored `scope_kind` column value.
///
/// Deliberately LOCAL rather than a reach for some engine `from_text`: the
/// engine exposes none, and the oracle's own decode is this same three-arm
/// match with everything else an `UnsupportedScope` refusal. A permissive
/// decode here would silently guard against the wrong scope.
/// @param text The stored column value.
/// @return The kind, or unset when the column holds something else.
auto scope_kind_from_column(std::string_view text) -> std::optional<engine::identity::scope_kind> {
  if (text == "global") {
    return engine::identity::scope_kind::global;
  }
  if (text == "association") {
    return engine::identity::scope_kind::association;
  }
  if (text == "repo") {
    return engine::identity::scope_kind::repo;
  }
  return std::nullopt;
}

/// @brief One pull-or-push result reduced to what the two renderers read.
///
/// `pull_result` and `push_result` are distinct engine types with identical
/// shapes; this is the single struct both render paths take, so the renderer
/// exists once rather than twice. It is NOT an attempt to merge the engine
/// types — they stay distinct there for the reason that module's header
/// gives.
struct rendered_result {
  std::int64_t                link_id = 0;                      ///< The link.
  sync_ns::outcome            result  = sync_ns::outcome::noop; ///< What happened.
  std::vector<std::string>    fields_changed;                   ///< Which fields differ.
  std::string                 detail;                           ///< Free text, or the error name.
  std::optional<std::string>  remote_title;  ///< EMITTED remote title (pull only; decision 996). Never written locally.
  std::optional<std::string>  remote_status; ///< EMITTED remote status (pull only; decision 996). Never written locally.
};

/// @brief Render one result as the oracle's JSON line.
///
/// `detail` is OMITTED entirely when empty rather than emitted as `""` or
/// `null` — same posture `ext list` takes with its optional columns, and
/// oracle-captured.
/// @param row The result.
/// @return The line, newline included.
auto render_result_json(const rendered_result& row) -> std::string {
  std::string out = std::format(R"({{"link_id":{},"outcome":)", row.link_id);
  // `outcome_to_event_text` is the `sync_events.outcome` column value AND
  // the Zig `@tagName` spelling; they coincide for all four enumerators, so
  // this reuses it rather than defining a second identical mapping.
  json_text::append_json_string(out, sync_ns::outcome_to_event_text(row.result));
  out += R"(,"fields_changed":[)";
  for (std::size_t i = 0; i < row.fields_changed.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    json_text::append_json_string(out, row.fields_changed[i]);
  }
  out += "]";
  if (!row.detail.empty()) {
    out += R"(,"detail":)";
    json_text::append_json_string(out, row.detail);
  }
  // EMITTED, not applied (decision 996): these carry the remote's values so
  // an agent can decide whether to write them through `planar`. Omitted
  // entirely when unset, same posture as `detail`.
  if (row.remote_title.has_value()) {
    out += R"(,"remote_title":)";
    json_text::append_json_string(out, *row.remote_title);
  }
  if (row.remote_status.has_value()) {
    out += R"(,"remote_status":)";
    json_text::append_json_string(out, *row.remote_status);
  }
  out += "}\n";
  return out;
}

/// @brief Render one result as the oracle's text line.
///
/// TWO leading spaces, and the separators are EM DASHES (U+2014), not
/// hyphens. The fields list is introduced by one em dash and then each field
/// is space-prefixed; a non-empty `detail` gets its own em dash after them.
/// Both separators are omitted when their part is empty, so a plain noop
/// renders as `  link 3: noop\n` with no trailing space.
/// @param row The result.
/// @return The line, newline included.
auto render_result_text(const rendered_result& row) -> std::string {
  std::string out = std::format("  link {}: {}", row.link_id, sync_ns::outcome_to_event_text(row.result));
  if (!row.fields_changed.empty()) {
    out += " —";
    for (auto const& field : row.fields_changed) {
      out += " ";
      out += field;
    }
  }
  if (!row.detail.empty()) {
    out += " — ";
    out += row.detail;
  }
  // EMITTED, not applied (decision 996). Rendered after `detail` so the
  // human-facing line still reads "outcome — fields — detail — remote:
  // ...".
  if (row.remote_title.has_value() || row.remote_status.has_value()) {
    out += " — remote:";
    if (row.remote_title.has_value()) {
      out += std::format(" title={:?}", *row.remote_title);
    }
    if (row.remote_status.has_value()) {
      out += std::format(" status={:?}", *row.remote_status);
    }
  }
  out += "\n";
  return out;
}

/// @brief Resolve which links a targeted or `--all` invocation acts on.
///
/// The positional accepts BOTH a bare link id and a `kind:id` entity
/// reference, and the discrimination is "does this parse as an integer" —
/// tried first, exactly as the oracle does, so a numeric-looking entity ref
/// could never be reached by falling through.
/// @param conn An open, migrated database connection.
/// @param all Whether `--all` was passed.
/// @param ref The positional, when passed.
/// @param pullable True for `pull` (read-only + two-way), false for `push`.
/// @param verb The verb name, for failure messages.
/// @return The links, or the failure.
auto resolve_target_links(db::connection& conn, bool all, std::optional<std::string> ref, bool pullable, std::string_view verb)
    -> std::expected<std::vector<link_ns::ext_link>, domain_error> {
  if (all) {
    auto rows = pullable ? link_ns::all_pullable(conn) : link_ns::all_pushable(conn);
    if (!rows) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                             std::format("{}: resolve target: {}", verb, link_error_name(rows.error()))));
    }
    return *rows;
  }
  // Unreachable through the parser (the caller refuses an absent ref before
  // calling), but the engine-facing contract stays total.
  if (!ref.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, "invalid sync target; expected <link-id> or <kind:id>"));
  }

  std::int64_t parsed_id  = 0;
  auto const*  first      = ref->data();
  auto const*  last       = first + ref->size();
  auto const   as_integer = std::from_chars(first, last, parsed_id);
  if (as_integer.ec == std::errc{} && as_integer.ptr == last) {
    auto one = link_ns::show(conn, parsed_id);
    if (!one) {
      if (one.error() == link_ns::link_error::not_found) {
        return std::unexpected(error_from_body(domain_error_kind::not_found, "sync target not found"));
      }
      return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                             std::format("{}: resolve target: {}", verb, link_error_name(one.error()))));
    }
    return std::vector<link_ns::ext_link>{*one};
  }

  auto const entity = parse_kind_id_ref(*ref);
  if (!entity.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, "invalid sync target; expected <link-id> or <kind:id>"));
  }
  auto rows = link_ns::links_for_entity(conn, entity->kind, entity->id);
  if (!rows) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("{}: resolve target: {}", verb, link_error_name(rows.error()))));
  }
  return *rows;
}

/// @brief Apply the cross-scope guard to every resolved link.
///
/// Called ONLY on the non-`--all` path; see sync.cppm for why `--all` is
/// deliberately unguarded.
/// @param ctx The invocation context.
/// @param conn An open, migrated database connection.
/// @param rows The links to guard.
/// @param scope_flag The raw `--scope` value, when passed.
/// @param verb The verb name, for failure messages.
/// @return Success, or the refusal.
auto guard_targets(context& ctx, db::connection& conn, std::span<const link_ns::ext_link> rows,
                   std::optional<std::string_view> scope_flag, std::string_view verb) -> std::expected<void, domain_error> {
  auto const resolution = resolve_write_scope(ctx, scope_flag, verb);
  if (!resolution) {
    return std::unexpected(resolution.error());
  }
  std::optional<std::string_view> write_scope;
  if (resolution->scope.has_value()) {
    write_scope = *resolution->scope;
  }
  for (auto const& row : rows) {
    auto const entity_scope = entity_scope_slug(conn, row);
    if (!entity_scope) {
      return std::unexpected(entity_scope.error());
    }
    std::optional<std::string_view> entity_view;
    if (entity_scope->has_value()) {
      entity_view = **entity_scope;
    }
    if (!guard_with_membership(conn, entity_view, write_scope)) {
      return std::unexpected(
          error_from_body(domain_error_kind::scope_mismatch, std::format("{} target is outside the operator write scope", verb)));
    }
  }
  return {};
}

/// @brief Drop every link that does not belong to `--system`'s row.
/// @param rows The links, consumed.
/// @param system_id The system to keep.
/// @return The kept links, in their original order.
auto filter_links_by_system(std::vector<link_ns::ext_link> rows, std::int64_t system_id) -> std::vector<link_ns::ext_link> {
  std::vector<link_ns::ext_link> kept;
  for (auto& row : rows) {
    if (row.system_id == system_id) {
      kept.push_back(std::move(row));
    }
  }
  return kept;
}

/// @brief Everything `pull` and `push` do identically, up to the one engine
/// call and the two exit arms.
///
/// Factored because the two oracle files are byte-for-byte identical for
/// their first ~60 lines and differ only in the verb name, `all_pullable` vs
/// `all_pushable`, the engine entry point, and whether a conflict is an exit
/// arm. Keeping two copies here would mean two places to drift.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param pullable True for `pull`.
/// @param verb The verb name, e.g. `"sync pull"`.
/// @return Success, or the failure to report.
auto run_pull_or_push(context& ctx, const cliapp::parsed_args& args, bool pullable, std::string_view verb) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const all      = cliapp::flag_bool(args, "--all");
  auto const as_json  = cliapp::flag_bool(args, "--json");
  auto const ref      = cliapp::positional_string(args, "ref");
  auto const system   = cliapp::flag_string(args, "--system");
  auto const scope_in = cliapp::flag_string(args, "--scope");

  if (!all && !ref.has_value()) {
    return std::unexpected(
        error_from_body(domain_error_kind::invalid_input, std::format("{} requires <link-id>, <kind:id>, or --all", verb)));
  }

  auto links = resolve_target_links(**conn, all, ref, pullable, verb);
  if (!links) {
    return std::unexpected(links.error());
  }

  // The guard runs BEFORE the `--system` filter, matching the oracle: a link
  // outside the write scope refuses the whole invocation even when
  // `--system` would have filtered it away. Reordering these would silently
  // make some refusals disappear.
  if (!all) {
    std::optional<std::string_view> scope_view;
    if (scope_in.has_value()) {
      scope_view = *scope_in;
    }
    auto const guarded = guard_targets(ctx, **conn, *links, scope_view, verb);
    if (!guarded) {
      return std::unexpected(guarded.error());
    }
  }

  if (system.has_value()) {
    auto const sys = system_ns::show_by_slug(**conn, *system);
    if (!sys) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                             std::format("{}: system '{}': {}", verb, *system, system_error_name(sys.error()))));
    }
    links = filter_links_by_system(std::move(*links), sys->id);
  }

  // Under `--json` an empty match set emits NOTHING AT ALL — not `[]`, not
  // a message. Only the text form has the "no links matched" line.
  if (!as_json && links->empty()) {
    ctx.out() << "no links matched\n";
    return {};
  }

  bool had_error     = false;
  bool conflict_seen = false;

  for (auto const& row : *links) {
    auto const sys = system_ns::show_by_id(**conn, row.system_id);
    if (!sys) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure,
                          std::format("{}: system {}: {}", verb, row.system_id, system_error_name(sys.error()))));
    }

    auto const built = build_adapter(*sys, default_deps(ctx.env()));
    if (!built) {
      // The RAW Zig error tag at exit 1, NOT `factory_error_message`'s prose
      // at exit 2 — see `factory_error_name`. A failure to build one
      // system's adapter aborts the whole run rather than becoming a
      // per-link result.
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure,
                          std::format("{}: build adapter for {}: {}", verb, sys->slug, factory_error_name(built.error()))));
    }

    rendered_result rendered{.link_id = row.id};
    if (pullable) {
      auto const done = sync_ns::pull_link(**conn, row, (*built)->instance());
      if (!done) {
        had_error       = true;
        rendered.result = sync_ns::outcome::error;
        rendered.detail = std::string(sync_error_name(done.error()));
      } else {
        rendered.result         = done->result;
        rendered.fields_changed = done->fields_changed;
        rendered.detail         = done->detail;
        rendered.remote_title   = done->remote_title;
        rendered.remote_status  = done->remote_status;
        if (done->result == sync_ns::outcome::conflict) {
          conflict_seen = true;
        }
      }
    } else {
      auto const done = sync_ns::push_link(**conn, row, (*built)->instance());
      if (!done) {
        had_error       = true;
        rendered.result = sync_ns::outcome::error;
        rendered.detail = std::string(sync_error_name(done.error()));
      } else {
        rendered.result         = done->result;
        rendered.fields_changed = done->fields_changed;
        rendered.detail         = done->detail;
      }
    }
    ctx.out() << (as_json ? render_result_json(rendered) : render_result_text(rendered));
  }

  if (had_error) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("one or more {} errors", pullable ? "pull" : "push")));
  }
  // Guarded on `!had_error` in the oracle too, which this reaches by
  // returning above. `push` never sets `conflict_seen`.
  if (conflict_seen) {
    return std::unexpected(
        error_from_body(domain_error_kind::sync_conflict, "one or more sync conflicts; use 'sync resolve' to settle"));
  }
  return {};
}

} // namespace

auto parse_kind_id_ref(std::string_view text) -> std::optional<kind_id_ref> {
  for (std::size_t i = text.size(); i > 0; --i) {
    if (text[i - 1] != ':') {
      continue;
    }
    auto const kind_text = text.substr(0, i - 1);
    auto const id_text   = text.substr(i);
    if (kind_text.empty() || id_text.empty()) {
      break;
    }
    auto const kind = link_ns::external_entity_kind_from_text(kind_text);
    if (!kind.has_value()) {
      break;
    }
    std::int64_t id    = 0;
    auto const   ended = std::from_chars(id_text.data(), id_text.data() + id_text.size(), id);
    if (ended.ec != std::errc{} || ended.ptr != id_text.data() + id_text.size()) {
      break;
    }
    return kind_id_ref{.kind = *kind, .id = id};
  }
  return std::nullopt;
}

// The parameter is spelled out rather than written through the `link_ns`
// alias so Doxygen matches this definition to its declaration in sync.cppm;
// through the alias it reads as a separate, undocumented member and the
// `cpp-lint` doxygen pass fails.
auto entity_scope_slug(db::connection& conn, const engine::external::link::ext_link& row)
    -> std::expected<std::optional<std::string>, domain_error> {
  auto const table = scope_table_for(row.entity_kind);
  if (!table.has_value()) {
    return std::optional<std::string>{};
  }

  // The table name is interpolated rather than bound because SQLite cannot
  // bind an identifier. It is safe here for a structural reason, not a
  // trusted-input one: `scope_table_for` maps a closed enum onto six
  // literals, so no operator-supplied byte can reach this string.
  auto statement = conn.prepare(std::format("select scope_kind, scope_id from {} where id = ?", *table));
  if (!statement) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync: resolving entity scope: QueryFailed"));
  }
  if (!statement->bind_int64(1, row.entity_id)) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync: resolving entity scope: QueryFailed"));
  }
  auto const stepped = statement->step();
  if (!stepped) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync: resolving entity scope: QueryFailed"));
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync: resolving entity scope: NotFound"));
  }

  auto const kind_text = statement->column_text(0);
  auto const kind      = scope_kind_from_column(kind_text);
  if (!kind.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync: resolving entity scope: UnsupportedScope"));
  }
  std::optional<std::int64_t> scope_id;
  if (*kind != engine::identity::scope_kind::global) {
    scope_id = statement->column_int64(1);
  }

  auto resolved = engine::identity::slug_from_ref(conn, *kind, scope_id);
  if (!resolved) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync: resolving entity scope: SlugNotFound"));
  }
  return *resolved;
}

// `guard_with_membership` moved to `planar.cmd.planar_ext.scope` at task 6303,
// when `feedback triage set` became its second caller family. The two call
// sites below are unchanged; this TU already imports that module for
// `resolve_write_scope`.

auto sync_pull(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_pull_or_push(ctx, args, true, "sync pull");
}

auto sync_push(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_pull_or_push(ctx, args, false, "sync push");
}

auto sync_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const raw_event = cliapp::positional_string(args, "event-id").value_or(std::string{});
  auto const keep_text = cliapp::flag_string(args, "--keep").value_or(std::string{});
  auto const token     = cliapp::flag_string(args, "--evidence-token").value_or(std::string{});
  auto const expected  = cliapp::flag_string(args, "--expected-local-updated-at").value_or(std::string{});
  auto const scope_in  = cliapp::flag_string(args, "--scope");
  auto const as_json   = cliapp::flag_bool(args, "--json");

  std::int64_t event_id = 0;
  auto const   ended    = std::from_chars(raw_event.data(), raw_event.data() + raw_event.size(), event_id);
  if (ended.ec != std::errc{} || ended.ptr != raw_event.data() + raw_event.size()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("invalid event-id '{}'", raw_event)));
  }

  sync_ns::resolve_keep keep{};
  if (keep_text == "local") {
    keep = sync_ns::resolve_keep::local;
  } else if (keep_text == "remote") {
    keep = sync_ns::resolve_keep::remote;
  } else {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                           std::format("--keep must be 'local' or 'remote', got '{}'", keep_text)));
  }

  // The link is read out of the EVENT rather than taken from the operator:
  // `sync resolve` names a conflict, and which link that conflict belongs to
  // is not the operator's to assert.
  auto link_statement = (*conn)->prepare("select link_id from sync_events where id = ?");
  if (!link_statement || !link_statement->bind_int64(1, event_id)) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync resolve: load event: QueryFailed"));
  }
  auto const stepped = link_statement->step();
  if (!stepped) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync resolve: load event: QueryFailed"));
  }
  if (*stepped != db::step_result::row) {
    return std::unexpected(error_from_body(domain_error_kind::not_found, std::format("sync event {} not found", event_id)));
  }
  auto const link_id = link_statement->column_int64(0);

  auto const row = link_ns::show(**conn, link_id);
  if (!row) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("sync resolve: load link {}: {}", link_id, link_error_name(row.error()))));
  }

  auto const entity_scope = entity_scope_slug(**conn, *row);
  if (!entity_scope) {
    return std::unexpected(entity_scope.error());
  }
  std::optional<std::string_view> scope_view;
  if (scope_in.has_value()) {
    scope_view = *scope_in;
  }
  auto const resolution = resolve_write_scope(ctx, scope_view, "sync resolve");
  if (!resolution) {
    return std::unexpected(resolution.error());
  }
  std::optional<std::string_view> entity_view;
  if (entity_scope->has_value()) {
    entity_view = **entity_scope;
  }
  std::optional<std::string_view> write_view;
  if (resolution->scope.has_value()) {
    write_view = *resolution->scope;
  }
  if (!guard_with_membership(**conn, entity_view, write_view)) {
    return std::unexpected(
        error_from_body(domain_error_kind::scope_mismatch, "sync conflict target is outside the operator write scope"));
  }

  auto const sys = system_ns::show_by_id(**conn, row->system_id);
  if (!sys) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure,
                        std::format("sync resolve: system {}: {}", row->system_id, system_error_name(sys.error()))));
  }

  auto const built = build_adapter(*sys, default_deps(ctx.env()));
  if (!built) {
    // Same raw-tag shape as pull/push, but WITHOUT the system slug: the
    // oracle's resolve handler interpolates only the error name here. The
    // three messages are not one shared string.
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("sync resolve: build adapter: {}", factory_error_name(built.error()))));
  }

  auto const done = sync_ns::resolve_conflict(**conn, event_id, keep, token, expected, (*built)->instance());
  if (!done) {
    // Four distinct refusals with THREE different exit codes between them:
    // `not_conflict` is operator error (2), the two CAS guards are conflicts
    // (3), and an adapter failure is generic (1). Collapsing any pair would
    // still pass a test that read only one of them.
    switch (done.error()) {
    case sync_ns::sync_error::not_conflict:
      return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                             std::format("sync event {} is not a conflict; nothing to resolve", event_id)));
    case sync_ns::sync_error::stale_conflict:
      return std::unexpected(
          error_from_body(domain_error_kind::sync_conflict,
                          std::format("sync event {} is stale or no longer the latest event for its link", event_id)));
    case sync_ns::sync_error::evidence_changed:
      return std::unexpected(error_from_body(
          domain_error_kind::sync_conflict,
          std::format("sync event {} evidence or approved local version changed; inspect and approve fresh evidence", event_id)));
    case sync_ns::sync_error::adapter_failed:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "sync resolve: adapter call failed"));
    default:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("sync resolve: {}", sync_error_name(done.error()))));
    }
  }

  if (as_json) {
    std::string out = std::format(R"({{"ok":{},"event_id":{},"new_event_id":{},"keep":)", done->ok ? "true" : "false", event_id,
                                  done->new_event_id);
    json_text::append_json_string(out, sync_ns::resolve_keep_to_text(keep));
    out += "}\n";
    ctx.out() << out;
    return {};
  }
  ctx.out() << std::format("resolved sync event {} (kept {}); new event {}\n", event_id, sync_ns::resolve_keep_to_text(keep),
                           done->new_event_id);
  return {};
}

auto sync_status(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const as_json = cliapp::flag_bool(args, "--json");
  auto const system  = cliapp::flag_string(args, "--system");
  auto const entity  = cliapp::flag_string(args, "--entity");

  // `--system` is NOT validated against `external_systems`; an unknown slug
  // is an empty result, not a refusal. See this leaf's header.
  link_ns::list_filter filter;
  if (system.has_value()) {
    filter.system_slug = *system;
  }
  if (entity.has_value()) {
    auto const ref = parse_kind_id_ref(*entity);
    if (!ref) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input,
                                             std::format("invalid --entity value '{}'; expected <kind>:<integer-id>", *entity)));
    }
    filter.entity_kind = ref->kind;
    filter.entity_id   = ref->id;
  }

  auto const rows = sync_ns::status(**conn, filter);
  if (!rows) {
    return std::unexpected(
        error_from_body(domain_error_kind::generic_failure, std::format("sync status: {}", sync_error_name(rows.error()))));
  }

  if (as_json) {
    // Line-delimited objects, no enclosing array. An empty set emits
    // nothing at all -- emitting `[]` here would be the natural-looking
    // mistake.
    std::string out;
    for (auto const& row : *rows) {
      out += std::format(R"({{"link_id":{},"entity_kind":)", row.link_id);
      json_text::append_json_string(out, link_ns::external_entity_kind_to_text(row.entity_kind));
      out += std::format(R"(,"entity_id":{},"external_id":)", row.entity_id);
      json_text::append_json_string(out, row.external_id);
      out += std::format(R"(,"system_id":{})", row.system_id);
      // Omitted entirely when NULL, rather than rendered as `null`.
      if (row.last_synced_at.has_value()) {
        out += R"(,"last_synced_at":)";
        json_text::append_json_string(out, *row.last_synced_at);
      }
      out += R"(,"last_sync_status":)";
      json_text::append_json_string(out, link_ns::sync_status_to_text(row.last_sync_status));
      out += "}\n";
    }
    ctx.out() << out;
    return {};
  }

  if (rows->empty()) {
    ctx.out() << "no external links\n";
    return {};
  }

  // Column widths are the oracle's `{s:<6} {s:<14} {s:<18} {s:<8} {s:<24}`
  // separated by TWO spaces each. On the data rows the entity column is not
  // one padded field but `<kind>:<id>` where only the ID carries the width,
  // so the rendered column is wider than its header whenever the kind is
  // longer than three characters. That is the oracle's, not a bug to fix
  // here.
  ctx.out() << std::format("{:<6}  {:<14}  {:<18}  {:<8}  {:<24}  {}\n", "link", "entity", "external-id", "system", "last-sync",
                           "status");
  std::string out;
  for (auto const& row : *rows) {
    auto const last = row.last_synced_at.has_value() ? std::string_view{*row.last_synced_at} : std::string_view{"never"};
    out += std::format("{:<6}  {}:{:<11}  {:<18}  {:<8}  {:<24}  {}\n", row.link_id,
                       link_ns::external_entity_kind_to_text(row.entity_kind), row.entity_id, row.external_id, row.system_id,
                       last, link_ns::sync_status_to_text(row.last_sync_status));
  }
  ctx.out() << out;
  return {};
}

} // namespace planar::cmd::ext::handlers

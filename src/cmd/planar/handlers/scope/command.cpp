/// @file src/cmd/planar/handlers/scope/command.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.scope`.

module planar.cmd.planar.handlers.scope;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.json_text;
import planar.engine.identity;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;
import planar.cmd.planar.declare;
import planar.cmd.planar.handlers.scope.show;
import planar.cmd.planar.handlers.scope.suggest;
import planar.cmd.planar.handlers.scope.use;
import planar.cmd.planar.handlers.scope.pop;
import planar.cmd.planar.handlers.scope.clear;

namespace planar::cmd::handlers {

namespace id = engine::identity;

namespace {

/// @brief The paragraph all three removed leaves print, with the verb
/// spliced in.
///
/// One function rather than three literals: the three oracle captures are
/// byte-identical apart from the verb, and three copies of a 240-character
/// sentence is exactly the drift D19 exists to prevent.
/// @param verb The verb as the operator typed it, e.g. `"scope use"`.
/// @return The refusal (exit 2 — `error.InvalidInput` in the oracle).
auto removed_in_m5(std::string_view verb) -> domain_error {
  return error_from_body(domain_error_kind::invalid_input,
                         std::format("`planar {}` was removed in plan 153 M5; the active scope stack is gone. Pass "
                                     "--scope <slug> to individual verbs, or cd into a registered scope. Run `planar "
                                     "scope show` to inspect the cwd-derived scope.",
                                     verb));
}

/// @brief One resolved read-set member, decorated with the row fields the
/// two renderers need.
///
/// Both renderers re-query `associations` / `projects` per member in the
/// oracle, and both coalesce a NULL slug to the empty string rather than
/// dropping the row. Resolved once here so the text and JSON paths cannot
/// drift apart on what they looked up.
struct decorated {
  id::scope_kind kind;          ///< The member's kind.
  std::int64_t   row_id = 0;    ///< The `associations` / `projects` row id.
  std::string    slug;          ///< `coalesce(slug,'')`.
  std::string    name;          ///< `coalesce(name,'')`; associations only.
  std::string    kind_label;    ///< `coalesce(kind,'')`; associations only.
  bool           found = false; ///< Whether the row still exists.
};

/// @brief Look up the row fields for one read-set member.
///
/// A VANISHED row is not an error: the oracle's `if ((try stmt.step()) ==
/// .row)` simply leaves the locals at `""`, and the renderers then fall
/// back to printing the numeric id. Reproduced rather than tightened —
/// refusing here would turn a stale read set into a hard failure where the
/// oracle degrades gracefully.
/// @param conn An open connection.
/// @param member The read-set member.
/// @return The decorated member, or `std::nullopt` on a SQL failure.
auto decorate(db::connection& conn, id::read_scope member) -> std::optional<decorated> {
  decorated out{.kind = member.kind, .row_id = member.id};
  if (member.kind == id::scope_kind::global) {
    out.found = true;
    return out;
  }

  auto stmt = member.kind == id::scope_kind::association
                  ? conn.prepare("select coalesce(slug,''), coalesce(name,''), coalesce(kind,'') from associations where id = ?")
                  : conn.prepare("select coalesce(slug,'') from projects where id = ?");
  if (!stmt) {
    return std::nullopt;
  }
  if (auto bound = stmt->bind_int64(1, member.id); !bound) {
    return std::nullopt;
  }
  auto step = stmt->step();
  if (!step) {
    return std::nullopt;
  }
  if (*step == db::step_result::row) {
    out.found = true;
    out.slug  = stmt->column_text(0);
    if (member.kind == id::scope_kind::association) {
      out.name       = stmt->column_text(1);
      out.kind_label = stmt->column_text(2);
    }
  }
  return out;
}

/// @brief Decorate the whole read set.
/// @param conn An open connection.
/// @param members The read set.
/// @return The decorated members, or a generic failure on SQL trouble.
auto decorate_all(db::connection& conn, std::span<const id::read_scope> members)
    -> std::expected<std::vector<decorated>, domain_error> {
  std::vector<decorated> out;
  out.reserve(members.size());
  for (auto const& member : members) {
    auto one = decorate(conn, member);
    if (!one) {
      return std::unexpected(error_from_body(domain_error_kind::generic_failure, "scope show: QueryFailed"));
    }
    out.push_back(std::move(*one));
  }
  return out;
}

/// @brief The trailer both non-JSON arms end with.
/// @param ctx The invocation context.
void print_trailer(context& ctx) {
  ctx.out() << "\n(The active scope stack was removed in plan 153 M5; scope is now derived from your current working "
               "directory.)\n";
}

/// @brief Render one decorated member as the text form's indented row.
///
/// The association arm's DOUBLE-PREFIX GUARD is the subtle part: the label
/// is the association's own `kind` column (falling back to `assoc` when
/// that is empty), and when the slug ALREADY starts with `<label>:` the
/// slug is printed alone. So an association of kind `client` with slug
/// `client:acme` renders `client:acme`, not `client:client:acme`.
/// @param ctx The invocation context.
/// @param row The decorated member.
void print_text_row(context& ctx, const decorated& row) {
  switch (row.kind) {
  case id::scope_kind::global:
    ctx.out() << "  global\n";
    return;
  case id::scope_kind::repo:
    if (!row.slug.empty()) {
      ctx.out() << std::format("  repo:{}\n", row.slug);
    } else {
      ctx.out() << std::format("  repo:{}\n", row.row_id);
    }
    return;
  case id::scope_kind::association: {
    auto const label = row.kind_label.empty() ? std::string_view{"assoc"} : std::string_view{row.kind_label};
    if (row.slug.empty()) {
      ctx.out() << std::format("  {}:{}\n", label, row.row_id);
      return;
    }
    auto const prefix = std::format("{}:", label);
    if (row.slug.starts_with(prefix)) {
      ctx.out() << std::format("  {}\n", row.slug);
    } else {
      ctx.out() << std::format("  {}:{}\n", label, row.slug);
    }
    return;
  }
  }
}

/// @brief Render one decorated member as a JSON object.
///
/// Every field is interpolated RAW, exactly as the oracle does — see this
/// module's header on the unescaped `cwd`. The `repo` arm hard-codes an
/// empty `name` and the literal `kind_label` `"repo"` rather than reading
/// either from the row; the `global` arm hard-codes all four.
/// @param row The decorated member.
/// @return The object's bytes.
auto json_row(const decorated& row) -> std::string {
  // Every string goes through `planar.json_text` (task 6254). These fields
  // used to be interpolated raw, so an association slug carrying a quote
  // emitted `"slug":"ev"il"` -- malformed, from the one flag whose entire
  // contract is that its output parses.
  switch (row.kind) {
  case id::scope_kind::global:
    // The global arm hard-codes all four values, so there is nothing here
    // that could need escaping. Left as a literal.
    return R"({"kind":"global","id":0,"slug":"","name":"","kind_label":"global"})";
  case id::scope_kind::association: {
    std::string out{std::format(R"({{"kind":"association","id":{},"slug":)", row.row_id)};
    json_text::append_json_string(out, row.slug);
    out += R"(,"name":)";
    json_text::append_json_string(out, row.name);
    out += R"(,"kind_label":)";
    json_text::append_json_string(out, row.kind_label);
    out += '}';
    return out;
  }
  case id::scope_kind::repo: {
    std::string out{std::format(R"({{"kind":"repo","id":{},"slug":)", row.row_id)};
    json_text::append_json_string(out, row.slug);
    out += R"(,"name":"","kind_label":"repo"})";
    return out;
  }
  }
  return {};
}

} // namespace

auto scope_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const cwd      = ctx.cwd().string();
  auto const override = cliapp::flag_string(args, "--scope");
  auto const override_view =
      override.has_value() ? std::optional<std::string_view>{*override} : std::optional<std::string_view>{};

  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto resolved = id::resolve_read_scope_set(**conn, cwd, override_view);
  if (!resolved) {
    if (resolved.error() == id::scope_error::slug_not_found) {
      // TWO stderr lines, and both are the oracle's. `resolveForReadSet`
      // prints the first itself — naming the slug the operator typed — and
      // then RETURNS the error, which main reports as the bare Zig tag. A
      // port that emitted only the second would drop the one line that says
      // WHICH slug was wrong; a port that emitted only the first would
      // change the tag every script greps for. Captured verbatim; `--scope
      // ""` takes this same path with an empty interpolation, because
      // unlike `--status` this flag is not comma-split and the empty value
      // does reach the resolver.
      return std::unexpected(error_from_rendered(
          domain_error_kind::generic_failure,
          std::format("error: resolving scope: scope slug not found: {}\nerror: SlugNotFound\n", override.value_or(""))));
    }
    return std::unexpected(map_scope_error(resolved.error(), "scope show"));
  }
  auto rows = decorate_all(**conn, *resolved);
  if (!rows) {
    return std::unexpected(rows.error());
  }

  if (cliapp::flag_bool(args, "--json")) {
    // `source` keys off the FLAG first, so `--scope <x>` reports `"flag"`
    // even when the resulting set is empty. Only a flagless invocation can
    // report `"cwd"` or `"none"`.
    auto const source = override.has_value() ? "flag" : (rows->empty() ? "none" : "cwd");
    ctx.out() << R"({"resolved_scopes":[)";
    for (std::size_t i = 0; i < rows->size(); ++i) {
      if (i > 0) {
        ctx.out() << ",";
      }
      ctx.out() << json_row((*rows)[i]);
    }
    // `cwd` is a FILESYSTEM PATH, so a backslash reaches this by accident
    // rather than by malice -- it is legal in a POSIX path component and it
    // is every separator on Windows, where the raw interpolation this
    // replaces made `scope show --json` broken by default rather than as an
    // edge case (task 6254).
    std::string tail{std::format(R"(],"source":"{}","cwd":)", source)};
    json_text::append_json_string(tail, cwd);
    tail += '}';
    ctx.out() << tail << '\n';
    return {};
  }

  if (!rows->empty()) {
    ctx.out() << (override.has_value() ? "resolved scope (from --scope flag):\n" : "resolved scope (from cwd):\n");
    for (auto const& row : *rows) {
      print_text_row(ctx, row);
    }
    print_trailer(ctx);
    return {};
  }

  // Empty read set: the three fallback arms, in the oracle's order.
  if (override.has_value()) {
    ctx.out() << "resolved scope (from --scope flag):\n" << std::format("  {}\n", *override);
    print_trailer(ctx);
    return {};
  }
  // Only NOW is the cwd derivation needed, and only on the no-override path.
  //
  // `derive_from_cwd` and NOT `resolve_write_scope`, deliberately. The
  // oracle's `scope show` calls `scope.zig`'s `resolve`, whose no-override
  // arm is a bare `deriveFromCwd`; `resolveForWrite` is the DIFFERENT
  // function, and the difference is not cosmetic — it consults the
  // meta-workspace table first and REFUSES at exit 5 on an ambiguous
  // workspace root. Routing `scope show` through it would make the one verb
  // an operator runs to UNDERSTAND an ambiguous scope fail with the very
  // ambiguity they ran it to diagnose. Verified against the oracle: `scope
  // show` never exits 5.
  auto resolution = id::derive_from_cwd(**conn, cwd);
  if (!resolution) {
    return std::unexpected(map_scope_error(resolution.error(), "scope show"));
  }
  if (resolution->scope.has_value()) {
    ctx.out() << "resolved scope (from cwd):\n" << std::format("  {}\n", *resolution->scope);
    print_trailer(ctx);
    return {};
  }

  switch (resolution->reason) {
  case id::derive_reason::no_project_match:
    ctx.out() << "resolved scope: none (cwd not inside any registered Planar scope)\n\n"
                 "cd into a registered scope or pass --scope <slug> to any verb.\n";
    break;
  case id::derive_reason::project_unassociated:
    ctx.out() << std::format("resolved scope: project '{}' has no association memberships (treated as global).\n\n",
                             resolution->project_slug.value_or("?"))
              << "Tag the project with `planar association add <slug> .` or run `planar scope suggest`.\n";
    break;
  case id::derive_reason::project_multiple_associations:
    ctx.out() << std::format("resolved scope: ambiguous — project '{}' belongs to multiple associations.\n\n",
                             resolution->project_slug.value_or("?"))
              << "Pass --scope <slug> to any verb to pick one explicitly.\n";
    break;
  case id::derive_reason::project_single_association:
    // The oracle marks this arm `unreachable`: a single association means
    // `resolution.scope` was set, which the branch above already took. A
    // C++ `unreachable()` here would be a crash on a state this port has
    // not proven impossible, so it degrades to the no-match text instead.
    ctx.out() << "resolved scope: none (cwd not inside any registered Planar scope)\n\n"
                 "cd into a registered scope or pass --scope <slug> to any verb.\n";
    break;
  }
  print_trailer(ctx);
  return {};
}

auto scope_suggest(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.db().ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto suggestions = id::suggest(**conn, ctx.cwd().string());
  if (!suggestions) {
    return std::unexpected(map_scope_error(suggestions.error(), "scope suggest"));
  }

  if (cliapp::flag_bool(args, "--json")) {
    // NDJSON, INCLUDING WHEN EMPTY (task 6257). There is no empty branch:
    // the loop below runs zero times and the verb writes zero bytes. It
    // used to short-circuit to `{"proposals":[]}` -- a key no populated
    // invocation ever emitted, wrapping an array it never produced, so a
    // consumer written against either shape broke on the other. See this
    // module's header for the family-wide rule.
    for (auto const& s : *suggestions) {
      std::string line{R"({"slug":)"};
      json_text::append_json_string(line, s.slug); // task 6254
      line += std::format(R"(,"association_id":{},"reason":)", s.association_id);
      json_text::append_json_string(line, s.reason);
      line += '}';
      ctx.out() << line << '\n';
    }
    return {};
  }

  if (suggestions->empty()) {
    ctx.out() << "no scope suggestions for cwd\n";
    return {};
  }
  ctx.out() << "suggested scope based on cwd:\n";
  for (auto const& s : *suggestions) {
    ctx.out() << std::format("  {}  ({})\n", s.slug, s.reason);
  }
  return {};
}

auto scope_use(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(ctx);
  static_cast<void>(args);
  return std::unexpected(removed_in_m5("scope use"));
}

auto scope_pop(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(ctx);
  static_cast<void>(args);
  return std::unexpected(removed_in_m5("scope pop"));
}

auto scope_clear(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  static_cast<void>(ctx);
  static_cast<void>(args);
  return std::unexpected(removed_in_m5("scope clear"));
}

auto declare_scope(CLI::App& root) -> void {
  CLI::App* scope = root.add_subcommand(
      "scope", "Inspect the scope Planar will resolve for the current working\n  directory.\n\n  Plan 153 removed the active "
               "scope stack; scope is now derived from\n  cwd and overridden by passing --scope <slug> to individual verbs.");
  scope->require_subcommand(0);

  scope_cli::attach_show(scope);

  scope_cli::attach_suggest(scope);

  scope_cli::attach_use(scope);

  scope_cli::attach_pop(scope);

  scope_cli::attach_clear(scope);
}

} // namespace planar::cmd::handlers

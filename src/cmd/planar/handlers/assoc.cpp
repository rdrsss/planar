/// @file assoc.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.assoc`.

module planar.cmd.planar.handlers.assoc;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.identity;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.declare;

namespace planar::cmd::handlers {

namespace id = engine::identity;

namespace {

/// @brief The Zig error name for an `association_error`.
///
/// zig's handler fails with `exit.die(ctx, e, "association create: {s}",
/// .{@errorName(e)})` — note "association create", NOT the "assoc create"
/// the operator typed; the message spells the ENGINE module's name and
/// the oracle capture confirms it (`error: association create:
/// SlugConflict`). Reproducing the operator's spelling here would look
/// tidier and break any script matching the real bytes.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(id::association_error err) -> std::string_view {
  switch (err) {
  case id::association_error::not_found:
    return "NotFound";
  case id::association_error::slug_conflict:
    return "SlugConflict";
  case id::association_error::unknown_kind:
    return "UnknownKind";
  case id::association_error::already_member:
    return "AlreadyMember";
  case id::association_error::not_a_member:
    return "NotAMember";
  case id::association_error::query_failed:
    return "QueryFailed";
  case id::association_error::audit_write_failed:
    // zig `policy.audit.Error` has the single member `WriteFailed`, and
    // the Zig call sites `try` it straight out of association.zig, so the
    // operator sees `association <verb>: WriteFailed`.
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Map an `association_error` onto this binary's exit-code bucket,
/// per zig/src/cmd/planar/exit.zig's `codeFor`.
///
/// `SlugConflict` maps to 6 (oracle-confirmed: a duplicate slug exits 6);
/// every other member has no arm in `codeFor` and falls to `else => 1`.
///
/// `engine_verb` is a PARAMETER because the two leaves that share this
/// mapping do not share the message. `assoc create` dies with `"association
/// create: {s}"` and `assoc add` with `"association add: {s}"` — and this
/// function was hard-coded to `create` when `assoc_add` first called it,
/// which surfaced as `error: association create: QueryFailed` from `assoc
/// add /`. Both spell the ENGINE module's verb, never the `assoc` the
/// operator typed.
/// @param err The engine error.
/// @param engine_verb The engine-side verb name, `"create"` or `"add"`.
/// @return The mapped failure.
auto map_association_error(id::association_error err, std::string_view engine_verb) -> domain_error {
  auto const kind =
      err == id::association_error::slug_conflict ? domain_error_kind::slug_conflict : domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("association {}: {}", engine_verb, zig_error_name(err)));
}

} // namespace

auto assoc_create(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before validating `--kind`. zig opens with `const d = try
  // runtime.ensureDb();`, so a refused `--kind nope` still creates and
  // migrates the database. Same ordering, same reason, as `plan create`'s.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto slug = cliapp::positional_string(args, "slug");
  if (!slug) {
    // Unreachable through the declared tree (the positional is required),
    // but the engine's `slug` is a non-optional `std::string` and an
    // empty default would write a blank-slug row. Refuse instead.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "assoc create: slug is required"));
  }

  id::create_args create{.slug = std::move(*slug), .name = cliapp::flag_string(args, "--name")};

  // Parse ONLY when the flag is present — an absent `--kind` leaves the
  // engine's `ad_hoc` default in place and must never reach the
  // unknown-kind refusal. Mirrors zig's `if (args.kind) |k| { ... }`.
  if (auto const kind_raw = cliapp::flag_string(args, "--kind")) {
    auto const kind = id::association_kind_from_text(*kind_raw);
    if (!kind) {
      // Exit 2 here, unlike `plan create`'s `--status` at 1. See this
      // module's header.
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown kind '{}'", *kind_raw)));
    }
    create.kind = *kind;
  }

  auto created = id::create(**conn, create);
  if (!created) {
    return std::unexpected(map_association_error(created.error(), "create"));
  }

  // Per-renderer terminator contract — see the two `@return` blocks on
  // `render_text` / `render_json` in `association.cppm`.
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << id::render_json(*created) << '\n';
  } else {
    ctx.out() << id::render_text(*created);
  }
  return {};
}

auto assoc_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto slug = cliapp::positional_string(args, "slug");
  auto path = cliapp::positional_string(args, "repo-path");
  // BOTH positionals are declared required, so neither is reachable as
  // unset through the CLI11 tree. Refusing anyway rather than defaulting:
  // an empty `repo_path` would register a `projects` row keyed on "" that
  // cwd-derive can never match again, which is the task-6128 shape (a row
  // written, exit 0, permanently wrong). `slug` is the positional NAME
  // from surface.cpp's `k_pos_54`, and `repo-path` is HYPHENATED there —
  // reading it as "repo_path" here would return unset on every call and
  // turn the whole verb into this refusal.
  if (!slug || !path) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "assoc add: slug and repo-path are required"));
  }

  // `*path` VERBATIM. No canonicalise, no absolute, no existence check —
  // see this function's declaration for why each of those would be a
  // silent regression rather than a hardening.
  auto added = id::add_member(**conn, *slug, *path, id::add_member_source::user);
  if (!added) {
    switch (added.error()) {
    case id::association_error::not_found:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("no association named '{}'", *slug)));
    case id::association_error::already_member:
      return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                             std::format("project at '{}' is already a member of '{}'", *path, *slug)));
    default:
      return std::unexpected(map_association_error(added.error(), "add"));
    }
  }

  if (cliapp::flag_bool(args, "--json")) {
    // Interpolated RAW, deliberately. The oracle builds this line with
    // `ctx.stdout.print("{{\"status\":\"added\",\"association\":\"{s}\"…")`
    // — a plain `{s}` substitution with no `output.writeJsonString` call —
    // so a slug or path containing a double quote produces INVALID JSON.
    // Captured, not inferred: `assoc add 'q"uote' '/tmp/pa"th' --json`
    // prints `{"status":"added","association":"q"uote","repo_path":"/tmp/pa"th"}`
    // on the oracle. Escaping here would be strictly better JSON and a
    // byte-level divergence from the binary this port is measured against;
    // if it is ever fixed it must be fixed on both sides at once.
    ctx.out() << std::format(R"({{"status":"added","association":"{}","repo_path":"{}"}})", *slug, *path) << '\n';
  } else {
    ctx.out() << std::format("added project at {} to {}\n", *path, *slug);
  }
  return {};
}

auto assoc_members(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto slug = cliapp::positional_string(args, "slug");
  if (!slug) {
    // Unreachable through the CLI11 tree (declared required). Refusing
    // rather than defaulting to "": an empty slug would resolve to no
    // association and print `(no members)`, which reads as "this
    // association is empty" rather than "you named nothing".
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "assoc members: slug is required"));
  }

  auto rows = id::members(**conn, *slug);
  if (!rows) {
    if (rows.error() == id::association_error::not_found) {
      // Same wording as `assoc add`'s unknown-association arm, and it is a
      // REFUSAL rather than an empty listing — see this leaf's declaration.
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("no association named '{}'", *slug)));
    }
    return std::unexpected(map_association_error(rows.error(), "members"));
  }

  // Terminator contract: the text renderer carries its own trailing
  // newline; the JSON one is a fragment this caller terminates.
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << id::render_member_list_json(*rows) << '\n';
  } else {
    ctx.out() << id::render_member_list_text(*rows);
  }
  return {};
}

auto assoc_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // Before the `--kind` check, same ordering (and same reason) as
  // `assoc_create`'s: zig opens with `try runtime.ensureDb()`, so a
  // refused `--kind nope` still creates and migrates the database.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  id::list_filter filter{};
  // PRESENCE, not emptiness. `--kind ''` is present and refuses; an absent
  // flag can never reach the refusal. See this leaf's declaration.
  if (auto const kind_raw = cliapp::flag_string(args, "--kind")) {
    auto const kind = id::association_kind_from_text(*kind_raw);
    if (!kind) {
      return std::unexpected(error_from_body(domain_error_kind::invalid_input, std::format("unknown kind '{}'", *kind_raw)));
    }
    filter.kind = *kind;
  }

  auto rows = id::list(**conn, filter);
  if (!rows) {
    return std::unexpected(map_association_error(rows.error(), "list"));
  }

  // Terminator contract: the text renderer carries its own trailing
  // newline; the JSON one is a fragment this caller terminates.
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << id::render_list_json(*rows) << '\n';
  } else {
    ctx.out() << id::render_list_text(*rows);
  }
  return {};
}

auto assoc_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto slug = cliapp::positional_string(args, "slug");
  // `repo-path` is HYPHENATED in surface.cpp's `k_pos_55`, exactly as it
  // is in `assoc add`'s `k_pos_54`. Reading it as "repo_path" returns
  // unset on every call and turns the whole verb into the refusal below.
  auto path = cliapp::positional_string(args, "repo-path");
  if (!slug || !path) {
    // Unreachable through the CLI11 tree (both declared required).
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "assoc remove: slug and repo-path are required"));
  }

  // `*path` VERBATIM — it is matched by string equality against whatever
  // `assoc add` stored, which is itself uncanonicalised. See this
  // function's declaration for the task-6256 shape this preserves.
  auto removed = id::remove_member(**conn, *slug, *path);
  if (!removed) {
    switch (removed.error()) {
    case id::association_error::not_found:
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("no association named '{}'", *slug)));
    case id::association_error::not_a_member:
      // Names the PATH and nothing else — not `assoc add`'s
      // already-a-member wording. See the declaration.
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("no project registered at '{}'", *path)));
    default:
      return std::unexpected(map_association_error(removed.error(), "remove"));
    }
  }

  if (cliapp::flag_bool(args, "--json")) {
    // Interpolated RAW, deliberately — same defect, deliberately
    // preserved, as `assoc_add`'s. See the declaration.
    ctx.out() << std::format(R"({{"status":"removed","association":"{}","repo_path":"{}"}})", *slug, *path) << '\n';
  } else {
    ctx.out() << std::format("removed project at {} from {}\n", *path, *slug);
  }
  return {};
}

auto assoc_detect(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // FIRST, before any probing -- zig opens with `try runtime.ensureDb()`,
  // so even a pure preview creates and migrates the database.
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const cwd       = ctx.cwd();
  auto const cwd_str   = cwd.string();
  auto       proposals = id::detect_proposals(cwd);

  // Enrich BEFORE deciding whether to apply, so the preview path and the
  // apply path share one annotation step. An unregistered cwd is fine here.
  if (auto enriched = id::enrich_proposals(**conn, proposals, cwd_str); !enriched) {
    return std::unexpected(map_association_error(enriched.error(), "detect"));
  }

  if (cliapp::flag_bool(args, "--apply")) {
    if (auto applied = id::apply_proposals(**conn, proposals, cwd_str); !applied) {
      if (applied.error() == id::association_error::not_found) {
        // Names the cwd and the remedy. The oracle's exact sentence,
        // backtick-free -- it spells the command bare.
        return std::unexpected(
            error_from_body(domain_error_kind::generic_failure,
                            std::format("no project registered at cwd ({}); run `planar init` first", cwd_str)));
      }
      return std::unexpected(map_association_error(applied.error(), "detect"));
    }
    // ORACLE: re-enrich after applying, and SWALLOW any failure (zig writes
    // `catch {}`). This is why a successful `--apply` prints `already a
    // member` for every row rather than the labels the preview showed.
    (void)id::enrich_proposals(**conn, proposals, cwd_str);
  }

  // Terminator contract: BOTH renderers now carry their own trailing
  // newlines and this caller adds none (task 6326). The JSON one used to be
  // a fragment this caller terminated, which is precisely why its empty
  // case could not be empty -- an unconditional `<< '\n'` here would have
  // turned zero proposals into a lone newline. See `render_detect_json`'s
  // declaration for the shape rule.
  if (cliapp::flag_bool(args, "--json")) {
    ctx.out() << id::render_detect_json(proposals);
  } else {
    ctx.out() << id::render_detect_text(proposals);
  }
  return {};
}

auto declare_assoc(CLI::App& root) -> void {
  CLI::App* assoc =
      root.add_subcommand("assoc", "Manage associations — the many-to-many tags that group repos into\n  named scopes.\n\n  Both "
                                   "'assoc' and 'association' are valid subcommand names.\n  User-creatable kinds: org, project, "
                                   "client, personal, ad-hoc.\n  Auto-detected kinds (via 'assoc detect'): host, path, lang.");
  assoc->require_subcommand(0);

  CLI::App* list = assoc->add_subcommand("list", "List all known associations.");
  add_string(*list, "--kind");
  add_json(*list);

  CLI::App* create = assoc->add_subcommand("create", "Create a new association.");
  add_string(*create, "--name");
  add_string(*create, "--kind");
  add_json(*create);
  add_positional(*create, "slug");

  CLI::App* add = assoc->add_subcommand("add", "Add a repo to an association.");
  add_json(*add);
  add_positional(*add, "slug");
  add_positional(*add, "repo-path");

  CLI::App* remove = assoc->add_subcommand("remove", "Remove a repo from an association.");
  add_json(*remove);
  add_positional(*remove, "slug");
  add_positional(*remove, "repo-path");

  CLI::App* members = assoc->add_subcommand("members", "List all project members of an association.");
  add_json(*members);
  add_positional(*members, "slug");

  CLI::App* detect = assoc->add_subcommand("detect", "Propose (or apply) auto-detected associations for the current directory.");
  add_bool(*detect, "--apply");
  add_json(*detect);
}

} // namespace planar::cmd::handlers

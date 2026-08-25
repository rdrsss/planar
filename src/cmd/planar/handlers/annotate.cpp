/// @file annotate.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.annotate`.

module planar.cmd.planar.handlers.annotate;

import std;
import cli11;
import planar.cliapp.args;
import planar.db;
import planar.engine.planning;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;
import planar.cmd.planar.scope;

namespace planar::cmd::handlers {

namespace ann = engine::planning::annotation;

namespace {

/// @brief The Zig error name for an `annotation_error`.
///
/// Both Zig handlers fail with `exit.die(ctx, e, "annotate <leaf>: {s}",
/// .{@errorName(e)})`, so the operator-visible message carries Zig's
/// CamelCase error TAG. The tags are transcribed from
/// zig/src/engine/planning/annotation.zig's `pub const Error` set, whose
/// seven members line up one-for-one with this port's
/// `annotation_error` — which is itself documented as mirroring it.
/// @param err The engine error.
/// @return The corresponding Zig error name.
auto zig_error_name(ann::annotation_error err) -> std::string_view {
  switch (err) {
  case ann::annotation_error::not_found:
    return "NotFound";
  case ann::annotation_error::unsupported_scope:
    return "UnsupportedScope";
  case ann::annotation_error::slug_not_found:
    return "SlugNotFound";
  case ann::annotation_error::terminal_status:
    return "TerminalStatus";
  case ann::annotation_error::slug_conflict:
    return "SlugConflict";
  case ann::annotation_error::empty_tag:
    return "EmptyTag";
  case ann::annotation_error::query_failed:
    return "QueryFailed";
  case ann::annotation_error::audit_write_failed:
    // zig `policy.audit.Error` has the single member `WriteFailed`, which
    // the Zig call sites `try` straight out of the engine module.
    return "WriteFailed";
  }
  return "Unknown";
}

/// @brief Map an `annotation_error` onto this binary's exit-code bucket,
/// per zig/src/cmd/planar/exit.zig's `codeFor`.
///
/// Only `SlugConflict` leaves the generic bucket (`codeFor` maps
/// `error.SlugConflict, error.AlreadyExists => 6`). The other six have no
/// arm in that switch and fall through to `else => 1` — including
/// `NotFound`, which parity-triage §F-exit-code-not-found deliberately
/// folded BACK into the generic-1 bucket so scripts can `|| exit 1`
/// cleanly. Mapping `not_found` to a distinct code here would look tidier
/// and be wrong.
/// @param err The engine error.
/// @param leaf The leaf name to lead the message with, e.g. `"annotate add"`.
/// @return The mapped failure.
auto map_annotation_error(ann::annotation_error err, std::string_view leaf) -> domain_error {
  auto const kind =
      err == ann::annotation_error::slug_conflict ? domain_error_kind::slug_conflict : domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("{}: {}", leaf, zig_error_name(err)));
}

/// @brief `map_annotation_error`, but rewriting `NotFound` to the
/// id-naming sentence the single-row leaves use.
///
/// The two shapes are NOT interchangeable and the split is per-leaf rather
/// than per-error. Captured from the oracle against a database with no row
/// 99:
///
///     annotate show 99     -> error: no annotation with id 99
///     annotate update 99   -> error: no annotation with id 99
///     annotate remove 99   -> error: no annotation with id 99
///     annotate resolve 99  -> error: no annotation with id 99
///     annotate dismiss 99  -> error: no annotation with id 99
///     annotate archive 99  -> error: no annotation with id 99
///     annotate tag 99 q    -> error: annotate tag: NotFound
///
/// So `tag` keeps `map_annotation_error` and its six siblings use this.
/// Every one of them exits 1 either way — the difference is wording an
/// operator greps, not a code a script branches on, which is exactly why
/// an exit-code diff would never have caught normalising them together.
/// @param err The engine error.
/// @param id The id the operator named, interpolated into the `NotFound`
/// message.
/// @param leaf The leaf name to lead a non-`NotFound` message with.
/// @return The mapped failure.
auto map_annotation_error_by_id(ann::annotation_error err, std::int64_t id, std::string_view leaf) -> domain_error {
  if (err == ann::annotation_error::not_found) {
    return error_from_body(domain_error_kind::generic_failure, std::format("no annotation with id {}", id));
  }
  return map_annotation_error(err, leaf);
}

/// @brief Split a `--tags` value on commas, trimming spaces and tabs and
/// dropping empties.
///
/// Mirrors zig/src/cmd/planar/handlers/annotate/add.zig verbatim: it
/// splits on ',', trims " \t", and skips anything that trims to empty —
/// de-duplication happens later, inside the engine. Oracle-confirmed with
/// `--tags "a, b,a"`, which stored the two tags `a` and `b`.
/// @param raw The raw flag value.
/// @return The trimmed, non-empty parts in input order.
auto split_tags(std::string_view raw) -> std::vector<std::string> {
  std::vector<std::string> out;
  for (auto const part : std::views::split(raw, ',')) {
    std::string_view piece{part.begin(), part.end()};
    while (!piece.empty() && (piece.front() == ' ' || piece.front() == '\t')) {
      piece.remove_prefix(1);
    }
    while (!piece.empty() && (piece.back() == ' ' || piece.back() == '\t')) {
      piece.remove_suffix(1);
    }
    if (!piece.empty()) {
      out.emplace_back(piece);
    }
  }
  return out;
}

/// @brief Convert an optional owned string to an optional view over it.
///
/// `annotation`'s arg structs take `std::optional<std::string_view>`, so
/// every value handed to them must be kept alive by a named local for the
/// duration of the call. Centralising the conversion keeps that discipline
/// visible at each call site rather than hidden in a temporary.
/// @param owned The owning optional; must outlive the returned view.
/// @return A view over `owned`, or unset.
auto as_view(const std::optional<std::string>& owned) -> std::optional<std::string_view> {
  if (!owned.has_value()) {
    return std::nullopt;
  }
  return std::string_view{*owned};
}

/// @brief Render one annotation on the `--json` or text path.
///
/// Both `render_json` and `render_text` are shared by six leaves (`add`,
/// `show`, `update`, `resolve`, `dismiss`, `archive`) and the terminator
/// rule differs between them — see this file's module header. Factored so
/// the `'\n'` is written once rather than six times, because six copies is
/// six chances to drop it on one leaf and produce output that differs from
/// the oracle by a single byte no `diff -q` on a `$(...)` capture would
/// ever show.
/// @param ctx The invocation context.
/// @param args The parsed arguments, read for `--json`.
/// @param a The annotation to render.
void emit_one(context& ctx, const cliapp::parsed_args& args, const ann::annotation& a) {
  if (flag_bool(args, "--json")) {
    ctx.out() << ann::render_json(a) << '\n';
  } else {
    ctx.out() << ann::render_text(a);
  }
}

/// @brief Read and validate `--status`.
///
/// The refusal wording and BUCKET are shared with `annotate list`: the Zig
/// handlers raise `error.InvalidStatus`, which has no arm in `exit.zig`'s
/// `codeFor` and so lands at exit 1, not the exit-2 user-input bucket the
/// message's shape suggests. Oracle-confirmed on `annotate update 7
/// --status bogus`: `error: unknown status 'bogus'`, exit 1.
/// @param args The parsed arguments.
/// @return The parsed status, unset when the flag is absent, or the
/// refusal.
auto status_flag(const cliapp::parsed_args& args) -> std::expected<std::optional<ann::status>, domain_error> {
  auto const raw = flag_string(args, "--status");
  if (!raw.has_value()) {
    return std::optional<ann::status>{};
  }
  auto const parsed = ann::status_from_text(*raw);
  if (!parsed.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", *raw)));
  }
  return std::optional<ann::status>{*parsed};
}

/// @brief The owned flag values a `list_filter` points into, plus the
/// filter itself.
///
/// `list_filter` holds `std::string_view`s, so every value handed to it
/// needs a named owner outliving the engine call. Bundling the two makes
/// that lifetime one object's rather than five locals' — a filter built
/// from temporaries compiles, runs, and reads freed memory, which surfaces
/// as a filter that matches nothing or matches everything depending on the
/// allocator's mood. Exactly the "plausible rows, wrong rows" failure this
/// cycle is guarding against, in its ugliest form.
struct filter_storage {
  std::optional<std::string> anchor_path; ///< Owned `--anchor-path`.
  std::optional<std::string> vendor;      ///< Owned `--vendor`.
  std::optional<std::string> tag;         ///< Owned `--tag`.
  std::optional<std::string> scope;       ///< Owned `--scope`.
  ann::list_filter           filter;      ///< The filter, viewing the members above.
};

/// @brief Build the selection filter the `bulk-*` and `verify` leaves
/// share, binding every view to storage that outlives the call.
///
/// `status_` is left UNSET here and set by the caller, because the three
/// bulk leaves genuinely disagree about it: `bulk-resolve` and
/// `bulk-dismiss` restrict to `active`, `bulk-archive` restricts to
/// nothing, and `verify` restricts to `active`. Defaulting it in one place
/// would have to pick a winner and silently wrong-foot the other two.
/// @param args The parsed arguments.
/// @return The storage, with `filter` bound to it.
auto shared_filter(const cliapp::parsed_args& args) -> std::unique_ptr<filter_storage> {
  auto store         = std::make_unique<filter_storage>();
  store->anchor_path = flag_string(args, "--anchor-path");
  store->vendor      = flag_string(args, "--vendor");
  store->tag         = flag_string(args, "--tag");
  store->scope       = flag_string(args, "--scope");
  store->filter      = ann::list_filter{
      .anchor_path = as_view(store->anchor_path),
      .status_     = std::nullopt,
      .plan_id     = flag_int(args, "--plan"),
      .task_id     = flag_int(args, "--task"),
      .vendor      = as_view(store->vendor),
      .tag         = as_view(store->tag),
      .scope       = as_view(store->scope),
  };
  return store;
}

/// @brief Run one `bulk-*` leaf.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param action Which transition to apply.
/// @param leaf The leaf name for a failure message, e.g. `"annotate bulk-resolve"`.
/// @param participle The past-participle noun the renderers take,
/// e.g. `"resolved"`.
/// @return Success, or the mapped engine failure.
auto run_bulk(context& ctx, const cliapp::parsed_args& args, ann::bulk_action action, std::string_view leaf,
              std::string_view participle) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto store = shared_filter(args);
  // `archive` legally progresses `resolved`/`dismissed` rows and therefore
  // takes NO status restriction; the other two take `active`. See
  // `annotate_bulk_archive`'s declaration for the oracle capture.
  if (action != ann::bulk_action::archive) {
    store->filter.status_ = ann::status::active;
  }

  auto const count = ann::bulk_apply(**conn, store->filter, action);
  if (!count) {
    return std::unexpected(map_annotation_error(count.error(), leaf));
  }

  if (flag_bool(args, "--json")) {
    ctx.out() << ann::render_bulk_json(participle, *count) << '\n';
  } else {
    ctx.out() << ann::render_bulk_text(participle, *count) << '\n';
  }
  return {};
}

/// @brief Run one of the `resolve` / `dismiss` / `archive` lifecycle leaves.
/// @param ctx The invocation context.
/// @param args The parsed arguments.
/// @param transition The engine entry point to call.
/// @param leaf The leaf name for a non-`NotFound` failure message.
/// @return Success, or `invalid_input` (exit 2) for a non-integer id, or
/// the mapped engine failure.
auto run_lifecycle(context& ctx, const cliapp::parsed_args& args,
                   std::expected<ann::annotation, ann::annotation_error> (*transition)(db::connection&, std::int64_t),
                   std::string_view leaf) -> handler_result {
  auto const id = entity_id_arg(args, "annotation-id", "annotation");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto updated = transition(**conn, *id);
  if (!updated) {
    return std::unexpected(map_annotation_error_by_id(updated.error(), *id, leaf));
  }
  emit_one(ctx, args, *updated);
  return {};
}

} // namespace

auto annotate_add(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  // The Zig handler refuses BEFORE resolving scope or touching the engine,
  // with a hand-written message rather than a parse error — `--anchor-path`
  // is declared optional in the tree and required by the handler. Captured:
  // stdout empty, stderr `error: --anchor-path is required`, exit 2.
  auto const anchor_path = flag_string(args, "--anchor-path");
  if (!anchor_path.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "--anchor-path is required"));
  }

  auto const scope_flag = flag_string(args, "--scope");
  auto       resolved   = resolve_write_scope(ctx, as_view(scope_flag), "annotate add");
  if (!resolved) {
    return std::unexpected(resolved.error());
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const title      = flag_string(args, "--title");
  auto const slug       = flag_string(args, "--slug");
  auto const body       = flag_string(args, "--body");
  auto const vendor     = flag_string(args, "--vendor");
  auto const commit_sha = flag_string(args, "--commit-sha");
  auto const text_hash  = flag_string(args, "--text-hash");
  auto const text       = flag_string(args, "--text");
  auto const tags_raw   = flag_string(args, "--tags");

  ann::create_args create{
      .anchor =
          {
              .path       = *anchor_path,
              .line_start = flag_int(args, "--line-start"),
              .line_end   = flag_int(args, "--line-end"),
              .commit_sha = commit_sha.value_or(std::string{}),
              .text_hash  = text_hash.value_or(std::string{}),
              .text       = text.value_or(std::string{}),
          },
      .title   = as_view(title),
      .slug    = as_view(slug),
      .body    = body.has_value() ? std::string_view{*body} : std::string_view{""},
      .vendor  = vendor.has_value() ? std::string_view{*vendor} : std::string_view{""},
      .plan_id = flag_int(args, "--plan"),
      .task_id = flag_int(args, "--task"),
      .tags    = tags_raw.has_value() ? split_tags(*tags_raw) : std::vector<std::string>{},
      .scope   = as_view(resolved->scope),
  };

  auto created = ann::create(**conn, create);
  if (!created) {
    return std::unexpected(map_annotation_error(created.error(), "annotate add"));
  }

  // render_json returns NO terminator (its own @return says so); render_text
  // includes them. See this leaf's module header.
  if (flag_bool(args, "--json")) {
    ctx.out() << ann::render_json(*created) << '\n';
  } else {
    ctx.out() << ann::render_text(*created);
  }
  return {};
}

auto annotate_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const anchor_path = flag_string(args, "--anchor-path");
  auto const vendor      = flag_string(args, "--vendor");
  auto const tag         = flag_string(args, "--tag");
  auto const scope       = flag_string(args, "--scope");
  auto const status_text = flag_string(args, "--status");

  ann::list_filter filter{
      .anchor_path = as_view(anchor_path),
      .status_     = std::nullopt,
      .plan_id     = flag_int(args, "--plan"),
      .task_id     = flag_int(args, "--task"),
      .vendor      = as_view(vendor),
      .tag         = as_view(tag),
      .scope       = as_view(scope),
  };
  if (status_text.has_value()) {
    auto const parsed = ann::status_from_text(*status_text);
    if (!parsed.has_value()) {
      // zig .../annotate/list.zig raises `error.InvalidStatus`, which has
      // no arm in exit.zig's `codeFor` and so lands in the generic-1
      // bucket — NOT the exit-2 user-input bucket the message's shape
      // might suggest.
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("unknown status '{}'", *status_text)));
    }
    filter.status_ = *parsed;
  }

  auto items = ann::list(**conn, filter);
  if (!items) {
    return std::unexpected(map_annotation_error(items.error(), "annotate list"));
  }

  if (flag_bool(args, "--json")) {
    ctx.out() << ann::render_list_json(*items) << '\n';
  } else {
    ctx.out() << ann::render_list_text(*items);
  }
  return {};
}

auto annotate_show(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const id = entity_id_arg(args, "annotation-id", "annotation");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto found = ann::show(**conn, *id);
  if (!found) {
    return std::unexpected(map_annotation_error_by_id(found.error(), *id, "annotate show"));
  }
  emit_one(ctx, args, *found);
  return {};
}

auto annotate_update(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const id = entity_id_arg(args, "annotation-id", "annotation");
  if (!id) {
    return std::unexpected(id.error());
  }
  // BEFORE the database opens, matching `annotate list`: an unrecognized
  // `--status` is a refusal that needs no connection, and the oracle
  // refuses `annotate update 99 --status bogus` with the status message
  // rather than the not-found one.
  auto const status_ = status_flag(args);
  if (!status_) {
    return std::unexpected(status_.error());
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const title = flag_string(args, "--title");
  auto const slug  = flag_string(args, "--slug");
  auto const body  = flag_string(args, "--body");
  auto const scope = flag_string(args, "--scope");

  ann::update_args patch{
      .title   = as_view(title),
      .slug    = as_view(slug),
      .body    = as_view(body),
      .status_ = *status_,
      .plan_id = flag_int(args, "--plan"),
      .task_id = flag_int(args, "--task"),
      // `annotate update` declares no anchor flags, so the anchor is never
      // replaced here. Passing a default-constructed `anchor_fields` would
      // blank the stored path, line range and text hash on every update —
      // an all-empty anchor is indistinguishable from "no anchor given"
      // once it reaches the UPDATE.
      .anchor = std::nullopt,
      .scope  = as_view(scope),
  };

  auto updated = ann::update(**conn, *id, patch);
  if (!updated) {
    return std::unexpected(map_annotation_error_by_id(updated.error(), *id, "annotate update"));
  }
  emit_one(ctx, args, *updated);
  return {};
}

auto annotate_remove(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const id = entity_id_arg(args, "annotation-id", "annotation");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }
  auto removed = ann::remove(**conn, *id);
  if (!removed) {
    return std::unexpected(map_annotation_error_by_id(removed.error(), *id, "annotate remove"));
  }
  // Both renderers document themselves as returning no terminator, and the
  // oracle writes 22 bytes for `annotation 12 removed` — the 21 characters
  // plus one newline. So this leaf appends on BOTH paths, unlike `show`.
  if (flag_bool(args, "--json")) {
    ctx.out() << ann::render_remove_json(*id) << '\n';
  } else {
    ctx.out() << ann::render_remove_text(*id) << '\n';
  }
  return {};
}

auto annotate_tag(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const id = entity_id_arg(args, "annotation-id", "annotation");
  if (!id) {
    return std::unexpected(id.error());
  }
  auto const tag = cliapp::positional_string(args, "tag");
  if (!tag.has_value()) {
    // Unreachable through the tree (declared required) but must never fall
    // through to tagging with the empty string, which the engine would
    // reject with `EmptyTag` and which would read here as a database
    // failure rather than a missing argument.
    return std::unexpected(error_from_body(domain_error_kind::invalid_input, "tag is required"));
  }

  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto const removing = flag_bool(args, "--remove");
  auto const applied  = removing ? ann::remove_tag(**conn, *id, *tag) : ann::add_tag(**conn, *id, *tag);
  if (!applied) {
    // NOT `map_annotation_error_by_id` — this leaf keeps the bare
    // `annotate tag: NotFound`. See its declaration.
    return std::unexpected(map_annotation_error(applied.error(), "annotate tag"));
  }

  if (flag_bool(args, "--json")) {
    ctx.out() << ann::render_tag_json(*id, *tag, removing) << '\n';
  } else {
    ctx.out() << ann::render_tag_text(*id, *tag, removing) << '\n';
  }
  return {};
}

auto annotate_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_lifecycle(ctx, args, &ann::resolve, "annotate resolve");
}

auto annotate_dismiss(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_lifecycle(ctx, args, &ann::dismiss, "annotate dismiss");
}

auto annotate_archive(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_lifecycle(ctx, args, &ann::archive, "annotate archive");
}

auto annotate_bulk_resolve(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_bulk(ctx, args, ann::bulk_action::resolve, "annotate bulk-resolve", "resolved");
}

auto annotate_bulk_dismiss(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_bulk(ctx, args, ann::bulk_action::dismiss, "annotate bulk-dismiss", "dismissed");
}

auto annotate_bulk_archive(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  return run_bulk(ctx, args, ann::bulk_action::archive, "annotate bulk-archive", "archived");
}

auto annotate_verify(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  auto store            = shared_filter(args);
  store->filter.status_ = ann::status::active;

  auto items = ann::list(**conn, store->filter);
  if (!items) {
    // This leaf's wording carries an extra segment no sibling has —
    // `annotate verify: list failed: SlugNotFound`. Oracle-captured.
    return std::unexpected(error_from_body(domain_error_kind::generic_failure,
                                           std::format("annotate verify: list failed: {}", zig_error_name(items.error()))));
  }

  std::vector<ann::verify_row> rows;
  rows.reserve(items->size());
  for (auto const& a : *items) {
    // Relative to the OPERATOR CWD, not the association root — see this
    // leaf's declaration for the experiment that settled it. An absolute
    // stored path wins on its own, which is what `operator/` does.
    auto const    full     = ctx.cwd() / std::filesystem::path{a.anchor.path};
    auto          contents = std::optional<std::string>{};
    std::ifstream in(full, std::ios::binary);
    if (in) {
      contents = std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    rows.push_back(ann::verify_row{
        .id          = a.id,
        .anchor_path = a.anchor.path,
        .state       = ann::classify_anchor(a.anchor.text_hash, contents.has_value() ? std::optional<std::string_view>{*contents}
                                                                                     : std::optional<std::string_view>{}),
    });
  }

  // `render_verify_text` documents itself as including a terminator on
  // every line; `render_verify_json` documents itself as omitting one.
  if (flag_bool(args, "--json")) {
    ctx.out() << ann::render_verify_json(rows) << '\n';
  } else {
    ctx.out() << ann::render_verify_text(rows);
  }
  return {};
}

auto annotate_sweep(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto conn = ctx.ensure_db();
  if (!conn) {
    return std::unexpected(conn.error());
  }

  // The tree declares `--since-days` with a default of `30`, and `harvest`
  // seeds an unpassed flag from that declared default — so the fallback
  // here is normally dead. It is written anyway, and written as 30 rather
  // than 0, because the two disagree destructively: `--since-days 0`
  // archives EVERY resolved and dismissed row, so a fallback of 0 reached
  // through a tree change would turn a routine `annotate sweep` into a
  // full-table archive that exits 0 and reports a plausible count.
  auto const since_days = flag_int(args, "--since-days").value_or(30);

  // `--scope` restricts the sweep and refuses an unresolvable slug with
  // exit 1, the same way `annotate list` and the three `bulk-*` leaves do.
  // Both trees changed together at plan 1001 / task 6150; before that the
  // flag parsed and was dropped on the floor, so a scoped sweep archived
  // every eligible row in the database and still exited 0 with a correct
  // count. `annotate sweep --scope nosuch` exiting 0 was the tell.
  auto const scope_flag = flag_string(args, "--scope");

  auto const swept = ann::sweep(**conn, since_days, as_view(scope_flag));
  if (!swept) {
    return std::unexpected(map_annotation_error(swept.error(), "annotate sweep"));
  }

  if (flag_bool(args, "--json")) {
    ctx.out() << ann::render_sweep_json(since_days, *swept) << '\n';
  } else {
    ctx.out() << ann::render_sweep_text(since_days, *swept) << '\n';
  }
  return {};
}

} // namespace planar::cmd::handlers

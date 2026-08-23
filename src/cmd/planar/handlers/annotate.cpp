/// @file annotate.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.annotate`.

module planar.cmd.planar.handlers.annotate;

import std;
import planar.cli;
import planar.engine.planning;
import planar.cmd.planar.args;
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
  auto const kind = err == ann::annotation_error::slug_conflict ? cli::domain_error_kind::slug_conflict
                                                                : cli::domain_error_kind::generic_failure;
  return error_from_body(kind, std::format("{}: {}", leaf, zig_error_name(err)));
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

} // namespace

auto annotate_add(context& ctx, const cli::match_result& args) -> handler_result {
  // The Zig handler refuses BEFORE resolving scope or touching the engine,
  // with a hand-written message rather than a parse error — `--anchor-path`
  // is declared optional in the tree and required by the handler. Captured:
  // stdout empty, stderr `error: --anchor-path is required`, exit 2.
  auto const anchor_path = flag_string(args, "--anchor-path");
  if (!anchor_path.has_value()) {
    return std::unexpected(error_from_body(cli::domain_error_kind::invalid_input, "--anchor-path is required"));
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

auto annotate_list(context& ctx, const cli::match_result& args) -> handler_result {
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
          error_from_body(cli::domain_error_kind::generic_failure, std::format("unknown status '{}'", *status_text)));
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

} // namespace planar::cmd::handlers

/// @file annotation.cpp
/// @brief Implementation of `planar.engine.planning.annotation` (plan 996,
/// task 6094). See annotation.cppm for scope, omissions, and the
/// oracle-derived output shapes.

module planar.engine.planning.annotation;

import std;
import planar.json_text;
import planar.db;
import planar.log;
import planar.scope_ref;
import planar.policy;
import planar.engine.planning.transitions;

namespace planar::engine::planning::annotation {

// The one shared escape table, layer 1. See json_text.cppm -- the local
// copy this replaced was missing the \b and \f short forms.
using json_text::json_string;

namespace audit = planar::policy::audit;

namespace {

/// @brief Append one `audit_log` row, mapping a write failure into this
/// module's error surface.
///
/// Called AFTER the mutation's own write, never before: a refused
/// annotate mutation writes no audit row in the oracle (verified by
/// running `annotate update <id> --status dismissed` against an already
/// `archived` row -- exit 1, `audit_log` untouched).
/// @param conn An open, migrated connection.
/// @param args The row to write.
/// @return Nothing, or `audit_write_failed`.
auto record_audit(db::connection& conn, const audit::record_args& args) -> std::expected<void, annotation_error> {
  if (auto ok = audit::record(conn, args); !ok) {
    return std::unexpected(annotation_error::audit_write_failed);
  }
  return {};
}

/// @brief Emit the oracle's inner `<op> exec failed: <ErrorName>` diagnostic
/// ahead of the outer handler error. See
/// `zig/src/engine/planning/annotation.zig`'s `create`/`update`/`remove`
/// for the shapes this ports; mirrors `engine::planning::exec_failed`
/// (task.cpp).
auto exec_failed(std::string_view op, std::string_view zig_error_name) -> annotation_error {
  log::diag_err(std::format("{} exec failed: {}", op, zig_error_name));
  return annotation_error::query_failed;
}

} // namespace

namespace {

// SQLITE_CONSTRAINT_UNIQUE — the same constant plan.cpp / task.cpp /
// entitylink.cpp already use to detect a UNIQUE violation without
// string-matching the driver's message.
constexpr int k_sqlite_constraint_unique = 2067;

auto is_unique_violation(const db::db_error& err) -> bool {
  return err.code_ == k_sqlite_constraint_unique;
}

constexpr std::string_view k_select_columns = "select id, scope_kind, scope_id, "
                                              "anchor_path, anchor_line_start, anchor_line_end, "
                                              "anchor_commit_sha, anchor_text_hash, anchor_text, "
                                              "title, slug, body, status, vendor, plan_id, task_id, "
                                              "created_at, updated_at from annotations";

/// @brief Trim ASCII whitespace the way zig's `std.mem.trim(u8, raw,
/// " \t\r\n")` does — exactly those four bytes, no locale involvement.
auto trim(std::string_view s) -> std::string_view {
  constexpr std::string_view k_ws = " \t\r\n";
  const auto                 b    = s.find_first_not_of(k_ws);
  if (b == std::string_view::npos) {
    return {};
  }
  const auto e = s.find_last_not_of(k_ws);
  return s.substr(b, e - b + 1);
}

auto opt_int(const db::statement& stmt, int index) -> std::optional<std::int64_t> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_int64(index);
}

auto opt_text(const db::statement& stmt, int index) -> std::optional<std::string> {
  if (stmt.is_null(index)) {
    return std::nullopt;
  }
  return stmt.column_text(index);
}

auto read_row(const db::statement& stmt) -> std::expected<annotation, annotation_error> {
  const auto kind = scope_kind_from_text(stmt.column_text(1));
  if (!kind.has_value()) {
    return std::unexpected(annotation_error::query_failed);
  }
  const auto st = status_from_text(stmt.column_text(12));
  if (!st.has_value()) {
    return std::unexpected(annotation_error::query_failed);
  }
  return annotation{
      .id          = stmt.column_int64(0),
      .scope_kind_ = *kind,
      .scope_id    = opt_int(stmt, 2),
      .anchor =
          anchor_fields{
              .path       = stmt.column_text(3),
              .line_start = opt_int(stmt, 4),
              .line_end   = opt_int(stmt, 5),
              .commit_sha = stmt.column_text(6),
              .text_hash  = stmt.column_text(7),
              .text       = stmt.column_text(8),
          },
      .title      = opt_text(stmt, 9),
      .slug       = opt_text(stmt, 10),
      .body       = stmt.column_text(11),
      .status_    = *st,
      .vendor     = stmt.column_text(13),
      .plan_id    = opt_int(stmt, 14),
      .task_id    = opt_int(stmt, 15),
      .tags       = {},
      .created_at = stmt.column_text(16),
      .updated_at = stmt.column_text(17),
  };
}

auto load_tags(db::connection& conn, std::int64_t ann_id) -> std::expected<std::vector<std::string>, annotation_error> {
  auto stmt = conn.prepare("select tag from annotation_tags where annotation_id = ? order by tag");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, ann_id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  std::vector<std::string> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    out.push_back(stmt->column_text(0));
  }
  return out;
}

/// @brief Resolve an optional scope-ref slug to the `(scope_kind text,
/// scope_id)` pair the annotation columns store. An unset slug means
/// global. Mirrors the `identity.scope.resolveSlug` call the Zig module
/// makes, routed through the layer-1 `planar.scope_ref` extraction (D19)
/// since `engine_identity` is a forbidden same-layer peer.
auto resolve_scope(db::connection& conn, std::optional<std::string_view> slug)
    -> std::expected<std::pair<scope_kind, std::optional<std::int64_t>>, annotation_error> {
  if (!slug.has_value()) {
    return std::pair<scope_kind, std::optional<std::int64_t>>{scope_kind::global, std::nullopt};
  }
  auto resolved = scope_ref::resolve(conn, *slug);
  if (!resolved) {
    switch (resolved.error()) {
    case scope_ref::error::slug_not_found:
      return std::unexpected(annotation_error::slug_not_found);
    case scope_ref::error::query_failed:
      return std::unexpected(annotation_error::query_failed);
    }
    return std::unexpected(annotation_error::query_failed);
  }
  switch (resolved->kind) {
  case scope_ref::scope_kind::global:
    return std::pair<scope_kind, std::optional<std::int64_t>>{scope_kind::global, resolved->id};
  case scope_ref::scope_kind::association:
    return std::pair<scope_kind, std::optional<std::int64_t>>{scope_kind::association, resolved->id};
  case scope_ref::scope_kind::repo:
    return std::pair<scope_kind, std::optional<std::int64_t>>{scope_kind::repo, resolved->id};
  }
  return std::unexpected(annotation_error::query_failed);
}

/// @brief The single `UPDATE annotations SET status = ?` both the three
/// lifecycle verbs and `update(status = ...)` funnel through.
auto write_status(db::connection& conn, std::int64_t id, status new_status) -> std::expected<void, annotation_error> {
  auto stmt = conn.prepare("update annotations set status = ?, "
                           "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, status_to_text(new_status)); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(2, id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(annotation_error::query_failed);
  }
  return {};
}

auto transition(db::connection& conn, std::int64_t id, status new_status, std::string_view verb_label)
    -> std::expected<annotation, annotation_error> {
  auto current = show(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  // Both `illegal_transition` AND `unknown_status` map to
  // `terminal_status`, mirroring zig's annotation.transition -- that
  // spelling is what bulk_apply and sweep observe.
  auto allowed =
      check_transition(transition_kind::annotation, status_to_text(current->status_), status_to_text(new_status), false);
  if (!allowed) {
    return std::unexpected(annotation_error::terminal_status);
  }
  if (auto w = write_status(conn, id, new_status); !w) {
    return std::unexpected(w.error());
  }
  // ORACLE: the summary is the VERB LABEL (`resolve` / `dismiss` /
  // `archive`), NOT the resulting status name -- captured as
  // `status_change|annotation|1|resolve`, not `...|resolved`. The two are
  // one letter apart and nothing rendered surfaces either.
  if (auto a = record_audit(conn, audit::record_args{.verb    = audit::verb::status_change,
                                                     .entity  = {.kind = "annotation", .id = id},
                                                     .summary = verb_label});
      !a) {
    return std::unexpected(a.error());
  }
  return show(conn, id);
}

// --- SHA-256 ---------------------------------------------------------------
//
// Needed only by `classify_anchor`, whose oracle behavior is a SHA-256 of
// the anchored file's bytes rendered as lowercase hex (verified: an
// annotation whose `--text-hash` was `shasum -a 256`'s output for the file
// classified `fresh`, one with a bogus hash classified `drifted`). No
// crypto library is vendored in this tree (`vendor/` holds catch2, glaze,
// spdlog and sqlite only), so the primitive is implemented here rather
// than pulling in a dependency for one leaf. Kept in an anonymous
// namespace so it is not part of the module's exported surface, and
// pinned against the FIPS 180-4 example vectors in annotation.t.cpp.

constexpr std::array<std::uint32_t, 64> k_sha256_k = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

auto rotr32(std::uint32_t x, unsigned n) -> std::uint32_t {
  return (x >> n) | (x << (32U - n));
}

auto sha256_hex(std::string_view data) -> std::string {
  std::array<std::uint32_t, 8> h = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

  std::vector<std::uint8_t> msg(data.begin(), data.end());
  const std::uint64_t       bit_len = static_cast<std::uint64_t>(data.size()) * 8U;
  msg.push_back(0x80U);
  while (msg.size() % 64U != 56U) {
    msg.push_back(0x00U);
  }
  for (int i = 7; i >= 0; --i) {
    msg.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFFU));
  }

  for (std::size_t off = 0; off < msg.size(); off += 64U) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16U; ++i) {
      w[i] = (static_cast<std::uint32_t>(msg[off + (i * 4) + 0]) << 24) |
             (static_cast<std::uint32_t>(msg[off + (i * 4) + 1]) << 16) |
             (static_cast<std::uint32_t>(msg[off + (i * 4) + 2]) << 8) | static_cast<std::uint32_t>(msg[off + (i * 4) + 3]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
      const auto s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i]          = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto a  = h[0];
    auto b  = h[1];
    auto c  = h[2];
    auto d  = h[3];
    auto e  = h[4];
    auto f  = h[5];
    auto g  = h[6];
    auto hh = h[7];
    for (std::size_t i = 0; i < 64U; ++i) {
      const auto s1    = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      const auto ch    = (e & f) ^ (~e & g);
      const auto temp1 = hh + s1 + ch + k_sha256_k[i] + w[i];
      const auto s0    = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      const auto maj   = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + maj;
      hh               = g;
      g                = f;
      f                = e;
      e                = d + temp1;
      d                = c;
      c                = b;
      b                = a;
      a                = temp1 + temp2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  std::string out;
  out.reserve(64);
  for (const auto word : h) {
    out += std::format("{:08x}", word);
  }
  return out;
}

// --- JSON helpers ----------------------------------------------------------

auto json_optional_int(std::optional<std::int64_t> v) -> std::string {
  return v.has_value() ? std::format("{}", *v) : std::string{"null"};
}

auto json_optional_string(const std::optional<std::string>& v) -> std::string {
  return v.has_value() ? json_string(*v) : std::string{"null"};
}

} // namespace

// ---------------------------------------------------------------------------
// Enum text
// ---------------------------------------------------------------------------

auto status_from_text(std::string_view s) -> std::optional<status> {
  if (s == "active") {
    return status::active;
  }
  if (s == "resolved") {
    return status::resolved;
  }
  if (s == "dismissed") {
    return status::dismissed;
  }
  if (s == "archived") {
    return status::archived;
  }
  return std::nullopt;
}

auto status_to_text(status s) -> std::string_view {
  switch (s) {
  case status::active:
    return "active";
  case status::resolved:
    return "resolved";
  case status::dismissed:
    return "dismissed";
  case status::archived:
    return "archived";
  }
  return "active"; // unreachable
}

auto scope_kind_from_text(std::string_view s) -> std::optional<scope_kind> {
  if (s == "global") {
    return scope_kind::global;
  }
  if (s == "repo") {
    return scope_kind::repo;
  }
  if (s == "association") {
    return scope_kind::association;
  }
  return std::nullopt;
}

auto scope_kind_to_text(scope_kind k) -> std::string_view {
  switch (k) {
  case scope_kind::global:
    return "global";
  case scope_kind::repo:
    return "repo";
  case scope_kind::association:
    return "association";
  }
  return "global"; // unreachable
}

auto is_terminal(status s) -> bool {
  return s == status::resolved || s == status::dismissed || s == status::archived;
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------

auto show(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error> {
  auto stmt = conn.prepare(std::format("{} where id = ?", k_select_columns));
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(annotation_error::not_found);
  }
  auto row = read_row(*stmt);
  if (!row) {
    return std::unexpected(row.error());
  }
  auto tags = load_tags(conn, row->id);
  if (!tags) {
    return std::unexpected(tags.error());
  }
  row->tags = std::move(*tags);
  return row;
}

auto show_by_slug(db::connection& conn, std::string_view slug) -> std::expected<annotation, annotation_error> {
  auto stmt = conn.prepare("select id from annotations where slug = ?");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_text(1, slug); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  auto step = stmt->step();
  if (!step) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (*step != db::step_result::row) {
    return std::unexpected(annotation_error::not_found);
  }
  return show(conn, stmt->column_int64(0));
}

auto create(db::connection& conn, const create_args& args) -> std::expected<annotation, annotation_error> {
  auto scope = resolve_scope(conn, args.scope);
  if (!scope) {
    return std::unexpected(scope.error());
  }

  auto stmt = conn.prepare("insert into annotations ("
                           "scope_kind, scope_id, "
                           "anchor_path, anchor_line_start, anchor_line_end, "
                           "anchor_commit_sha, anchor_text_hash, anchor_text, "
                           "title, slug, body, status, vendor, plan_id, task_id"
                           ") values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) returning id");
  if (!stmt) {
    return std::unexpected(exec_failed("annotation.create", "PrepareFailed"));
  }

  const auto bind_opt_text = [&](int index, std::optional<std::string_view> v) {
    return v.has_value() ? stmt->bind_text(index, *v).has_value() : stmt->bind_null(index).has_value();
  };
  const auto bind_opt_int = [&](int index, std::optional<std::int64_t> v) {
    return v.has_value() ? stmt->bind_int64(index, *v).has_value() : stmt->bind_null(index).has_value();
  };

  const bool bound = stmt->bind_text(1, scope_kind_to_text(scope->first)).has_value() && bind_opt_int(2, scope->second) &&
                     stmt->bind_text(3, args.anchor.path).has_value() && bind_opt_int(4, args.anchor.line_start) &&
                     bind_opt_int(5, args.anchor.line_end) && stmt->bind_text(6, args.anchor.commit_sha).has_value() &&
                     stmt->bind_text(7, args.anchor.text_hash).has_value() && stmt->bind_text(8, args.anchor.text).has_value() &&
                     bind_opt_text(9, args.title) && bind_opt_text(10, args.slug) && stmt->bind_text(11, args.body).has_value() &&
                     stmt->bind_text(12, status_to_text(args.status_)).has_value() &&
                     stmt->bind_text(13, args.vendor).has_value() && bind_opt_int(14, args.plan_id) &&
                     bind_opt_int(15, args.task_id);
  if (!bound) {
    return std::unexpected(exec_failed("annotation.create", "BindFailed"));
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(annotation_error::slug_conflict);
    }
    return std::unexpected(exec_failed("annotation.create", "StepFailed"));
  }
  if (*step != db::step_result::row) {
    return std::unexpected(exec_failed("annotation.create", "StepFailed"));
  }
  const auto id = stmt->column_int64(0);

  // Tags: trim, drop empties, de-duplicate -- in FIRST-SEEN order, which
  // does not matter for readback since every read sorts by tag.
  std::vector<std::string_view> seen;
  for (const auto& raw : args.tags) {
    const auto t = trim(raw);
    if (t.empty()) {
      continue;
    }
    if (std::ranges::find(seen, t) != seen.end()) {
      continue;
    }
    seen.push_back(t);
    auto ins = conn.prepare("insert into annotation_tags (annotation_id, tag) values (?, ?)");
    if (!ins) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto b = ins->bind_int64(1, id); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto b = ins->bind_text(2, t); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto s = ins->step(); !s) {
      return std::unexpected(annotation_error::query_failed);
    }
  }

  // ORACLE: `create annotation 'A1'` -- the TITLE in single quotes, and
  // the anchor path when there is no title. Captured against a titled row
  // (`create annotation 'A-alpha'`) and an untitled one.
  const auto summary = std::format("create annotation '{}'", args.title.value_or(std::string_view{args.anchor.path}));
  if (auto a = record_audit(
          conn, audit::record_args{.verb = audit::verb::create, .entity = {.kind = "annotation", .id = id}, .summary = summary});
      !a) {
    return std::unexpected(a.error());
  }

  return show(conn, id);
}

auto list(db::connection& conn, const list_filter& filter) -> std::expected<std::vector<annotation>, annotation_error> {
  std::optional<std::pair<scope_kind, std::optional<std::int64_t>>> scope;
  if (filter.scope.has_value()) {
    auto resolved = resolve_scope(conn, filter.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope = *resolved;
  }

  std::string sql = std::format("{} where 1 = 1", k_select_columns);
  if (filter.anchor_path.has_value()) {
    sql += " and anchor_path = ?";
  }
  if (filter.status_.has_value()) {
    sql += " and status = ?";
  }
  if (filter.plan_id.has_value()) {
    sql += " and plan_id = ?";
  }
  if (filter.task_id.has_value()) {
    sql += " and task_id = ?";
  }
  if (filter.vendor.has_value()) {
    sql += " and vendor = ?";
  }
  if (filter.tag.has_value()) {
    sql += " and id in (select annotation_id from annotation_tags where tag = ?)";
  }
  if (scope.has_value()) {
    switch (scope->first) {
    case scope_kind::global:
      sql += " and scope_kind = 'global'";
      break;
    case scope_kind::association:
      sql += " and scope_kind = 'association' and scope_id = ?";
      break;
    case scope_kind::repo:
      sql += " and scope_kind = 'repo' and scope_id = ?";
      break;
    }
  }
  sql += " order by id";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  int idx = 1;
  if (filter.anchor_path.has_value()) {
    if (auto b = stmt->bind_text(idx++, *filter.anchor_path); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.status_.has_value()) {
    if (auto b = stmt->bind_text(idx++, status_to_text(*filter.status_)); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.plan_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.plan_id); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.task_id.has_value()) {
    if (auto b = stmt->bind_int64(idx++, *filter.task_id); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.vendor.has_value()) {
    if (auto b = stmt->bind_text(idx++, *filter.vendor); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (filter.tag.has_value()) {
    if (auto b = stmt->bind_text(idx++, *filter.tag); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }
  if (scope.has_value() && scope->first != scope_kind::global) {
    if (!scope->second.has_value()) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto b = stmt->bind_int64(idx++, *scope->second); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }

  std::vector<annotation> out;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    auto row = read_row(*stmt);
    if (!row) {
      return std::unexpected(row.error());
    }
    out.push_back(std::move(*row));
  }
  // Tags are a follow-up query per row, matching the Zig original (which
  // also cannot nest a second statement inside its own step loop).
  for (auto& row : out) {
    auto tags = load_tags(conn, row.id);
    if (!tags) {
      return std::unexpected(tags.error());
    }
    row.tags = std::move(*tags);
  }
  return out;
}

auto update(db::connection& conn, std::int64_t id, const update_args& patch) -> std::expected<annotation, annotation_error> {
  std::optional<std::pair<scope_kind, std::optional<std::int64_t>>> scope;
  if (patch.scope.has_value()) {
    auto resolved = resolve_scope(conn, patch.scope);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope = *resolved;
  }

  auto current = show(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }

  if (patch.status_.has_value()) {
    auto allowed =
        check_transition(transition_kind::annotation, status_to_text(current->status_), status_to_text(*patch.status_), false);
    if (!allowed) {
      return std::unexpected(annotation_error::terminal_status);
    }
  }

  std::string sql   = "update annotations set ";
  bool        first = true;
  const auto  sep   = [&]() {
    if (!first) {
      sql += ", ";
    }
    first = false;
  };

  if (scope.has_value()) {
    sep();
    sql += "scope_kind = ?";
    sep();
    sql += "scope_id = ?";
  }
  if (patch.title.has_value()) {
    sep();
    sql += "title = ?";
  }
  if (patch.slug.has_value()) {
    sep();
    sql += "slug = ?";
  }
  if (patch.body.has_value()) {
    sep();
    sql += "body = ?";
  }
  if (patch.status_.has_value()) {
    sep();
    sql += "status = ?";
  }
  if (patch.plan_id.has_value()) {
    sep();
    sql += "plan_id = ?";
  }
  if (patch.task_id.has_value()) {
    sep();
    sql += "task_id = ?";
  }
  if (patch.anchor.has_value()) {
    sep();
    sql += "anchor_path = ?, anchor_line_start = ?, anchor_line_end = ?, "
           "anchor_commit_sha = ?, anchor_text_hash = ?, anchor_text = ?";
  }

  // An all-unset patch is a no-op that returns a FRESH SNAPSHOT and does
  // NOT bump updated_at -- matching zig's `if (first) return try show(...)`.
  // This return is BEFORE the audit write below on purpose: a bare
  // `annotate update <id>` writes no `audit_log` row in the oracle either
  // (verified by running it and reading the table back -- the row's
  // `create` entry was the only one present afterwards).
  if (first) {
    return show(conn, id);
  }

  sep();
  sql += "updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') where id = ?";

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(exec_failed("annotation.update", "PrepareFailed"));
  }
  int  idx   = 1;
  bool bound = true;
  if (scope.has_value()) {
    bound = bound && stmt->bind_text(idx++, scope_kind_to_text(scope->first)).has_value();
    bound = bound && (scope->second.has_value() ? stmt->bind_int64(idx++, *scope->second).has_value()
                                                : stmt->bind_null(idx++).has_value());
  }
  if (patch.title.has_value()) {
    bound = bound && stmt->bind_text(idx++, *patch.title).has_value();
  }
  if (patch.slug.has_value()) {
    bound = bound && stmt->bind_text(idx++, *patch.slug).has_value();
  }
  if (patch.body.has_value()) {
    bound = bound && stmt->bind_text(idx++, *patch.body).has_value();
  }
  if (patch.status_.has_value()) {
    bound = bound && stmt->bind_text(idx++, status_to_text(*patch.status_)).has_value();
  }
  if (patch.plan_id.has_value()) {
    bound = bound && stmt->bind_int64(idx++, *patch.plan_id).has_value();
  }
  if (patch.task_id.has_value()) {
    bound = bound && stmt->bind_int64(idx++, *patch.task_id).has_value();
  }
  if (patch.anchor.has_value()) {
    const auto& a = *patch.anchor;
    bound         = bound && stmt->bind_text(idx++, a.path).has_value();
    bound = bound &&
            (a.line_start.has_value() ? stmt->bind_int64(idx++, *a.line_start).has_value() : stmt->bind_null(idx++).has_value());
    bound =
        bound && (a.line_end.has_value() ? stmt->bind_int64(idx++, *a.line_end).has_value() : stmt->bind_null(idx++).has_value());
    bound = bound && stmt->bind_text(idx++, a.commit_sha).has_value();
    bound = bound && stmt->bind_text(idx++, a.text_hash).has_value();
    bound = bound && stmt->bind_text(idx++, a.text).has_value();
  }
  bound = bound && stmt->bind_int64(idx, id).has_value();
  if (!bound) {
    return std::unexpected(exec_failed("annotation.update", "BindFailed"));
  }

  auto step = stmt->step();
  if (!step) {
    if (is_unique_violation(step.error())) {
      return std::unexpected(annotation_error::slug_conflict);
    }
    return std::unexpected(exec_failed("annotation.update", "StepFailed"));
  }
  // ORACLE: a patch that carries a status writes `status_change`; any
  // other patch writes `update`. BOTH carry a NULL summary -- verified by
  // running `annotate update 1 --status resolved` and `annotate update 2
  // --title T2 --status dismissed`, each of which produced
  // `status_change|annotation|<id>|<NULL>`. The verb-labelled summaries
  // come only from the dedicated `resolve`/`dismiss`/`archive` verbs.
  if (auto a = record_audit(
          conn, audit::record_args{.verb   = patch.status_.has_value() ? audit::verb::status_change : audit::verb::update,
                                   .entity = {.kind = "annotation", .id = id}});
      !a) {
    return std::unexpected(a.error());
  }
  return show(conn, id);
}

auto remove(db::connection& conn, std::int64_t id) -> std::expected<void, annotation_error> {
  // Verify existence first, so a missing id is not_found rather than a
  // silent no-op.
  auto current = show(conn, id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto stmt = conn.prepare("delete from annotations where id = ?");
  if (!stmt) {
    return std::unexpected(exec_failed("annotation.remove", "PrepareFailed"));
  }
  if (auto b = stmt->bind_int64(1, id); !b) {
    return std::unexpected(exec_failed("annotation.remove", "BindFailed"));
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(exec_failed("annotation.remove", "StepFailed"));
  }
  // ORACLE: `delete` with a NULL summary -- the deleted row's title is NOT
  // interpolated, unlike `create`.
  return record_audit(conn, audit::record_args{.verb = audit::verb::delete_, .entity = {.kind = "annotation", .id = id}});
}

auto resolve(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error> {
  return transition(conn, id, status::resolved, "resolve");
}

auto dismiss(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error> {
  return transition(conn, id, status::dismissed, "dismiss");
}

auto archive(db::connection& conn, std::int64_t id) -> std::expected<annotation, annotation_error> {
  return transition(conn, id, status::archived, "archive");
}

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------

auto add_tag(db::connection& conn, std::int64_t ann_id, std::string_view tag) -> std::expected<void, annotation_error> {
  const auto trimmed = trim(tag);
  if (trimmed.empty()) {
    return std::unexpected(annotation_error::empty_tag);
  }
  auto current = show(conn, ann_id);
  if (!current) {
    return std::unexpected(current.error());
  }
  auto stmt = conn.prepare("insert into annotation_tags (annotation_id, tag) values (?, ?) "
                           "on conflict(annotation_id, tag) do nothing");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, ann_id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_text(2, trimmed); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(annotation_error::query_failed);
  }
  return {};
}

auto remove_tag(db::connection& conn, std::int64_t ann_id, std::string_view tag) -> std::expected<void, annotation_error> {
  const auto trimmed = trim(tag);
  if (trimmed.empty()) {
    return std::unexpected(annotation_error::empty_tag);
  }
  auto stmt = conn.prepare("delete from annotation_tags where annotation_id = ? and tag = ?");
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_int64(1, ann_id); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto b = stmt->bind_text(2, trimmed); !b) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (auto s = stmt->step(); !s) {
    return std::unexpected(annotation_error::query_failed);
  }
  return {};
}

auto list_tags(db::connection& conn, std::int64_t ann_id) -> std::expected<std::vector<std::string>, annotation_error> {
  return load_tags(conn, ann_id);
}

// ---------------------------------------------------------------------------
// bulk
// ---------------------------------------------------------------------------

auto bulk_apply(db::connection& conn, const list_filter& filter, bulk_action action)
    -> std::expected<std::size_t, annotation_error> {
  auto items = list(conn, filter);
  if (!items) {
    return std::unexpected(items.error());
  }

  std::size_t count = 0;
  for (const auto& a : *items) {
    const bool already_done = (action == bulk_action::resolve && a.status_ == status::resolved) ||
                              (action == bulk_action::dismiss && a.status_ == status::dismissed) ||
                              (action == bulk_action::archive && a.status_ == status::archived);
    if (already_done) {
      continue;
    }
    // resolve/dismiss cannot accept a row at ANY outcome state.
    // archive deliberately does NOT pre-skip: under the retention-tier
    // model resolved and dismissed legally progress to archived.
    if (action != bulk_action::archive && is_terminal(a.status_)) {
      continue;
    }

    std::expected<annotation, annotation_error> updated = action == bulk_action::resolve   ? resolve(conn, a.id)
                                                          : action == bulk_action::dismiss ? dismiss(conn, a.id)
                                                                                           : archive(conn, a.id);
    if (!updated) {
      if (updated.error() == annotation_error::terminal_status) {
        continue;
      }
      // NOT transactional: every row already updated stays updated. See
      // annotation.cppm's `bulk_apply` doc comment for the oracle probe
      // that established this.
      return std::unexpected(updated.error());
    }
    ++count;
  }
  return count;
}

auto render_bulk_json(std::string_view verb_name, std::size_t count) -> std::string {
  return std::format(R"({{"ok":true,"action":{},"count":{}}})", json_string(verb_name), count);
}

auto render_bulk_text(std::string_view verb_name, std::size_t count) -> std::string {
  return std::format("{}: {} annotation(s)", verb_name, count);
}

// ---------------------------------------------------------------------------
// sweep
// ---------------------------------------------------------------------------

auto sweep_candidates(db::connection& conn, std::int64_t since_days, std::optional<std::string_view> scope_slug)
    -> std::expected<std::vector<std::int64_t>, annotation_error> {
  // Resolve BEFORE building the statement so an unresolvable slug refuses
  // without archiving anything. `resolve_scope(conn, nullopt)` means
  // "global", which is a filter; "no filter" is the absent optional, so the
  // two are kept apart exactly as `list` keeps them apart.
  std::optional<std::pair<scope_kind, std::optional<std::int64_t>>> scope;
  if (scope_slug.has_value()) {
    auto resolved = resolve_scope(conn, scope_slug);
    if (!resolved) {
      return std::unexpected(resolved.error());
    }
    scope = *resolved;
  }

  // The cutoff is interpolated rather than bound, matching the Zig
  // original's `bufPrintZ`; `since_days` is an integer, so no injection
  // surface exists. The scope id is bound.
  std::string sql = std::format("select id from annotations"
                                " where status in ('resolved','dismissed')"
                                "   and (julianday('now') - julianday(updated_at)) > {}",
                                since_days);
  if (scope.has_value()) {
    switch (scope->first) {
    case scope_kind::global:
      sql += " and scope_kind = 'global'";
      break;
    case scope_kind::association:
      sql += " and scope_kind = 'association' and scope_id = ?";
      break;
    case scope_kind::repo:
      sql += " and scope_kind = 'repo' and scope_id = ?";
      break;
    }
  }

  auto stmt = conn.prepare(sql);
  if (!stmt) {
    return std::unexpected(annotation_error::query_failed);
  }
  if (scope.has_value() && scope->first != scope_kind::global) {
    if (!scope->second.has_value()) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (auto b = stmt->bind_int64(1, *scope->second); !b) {
      return std::unexpected(annotation_error::query_failed);
    }
  }

  std::vector<std::int64_t> ids;
  while (true) {
    auto step = stmt->step();
    if (!step) {
      return std::unexpected(annotation_error::query_failed);
    }
    if (*step != db::step_result::row) {
      break;
    }
    ids.push_back(stmt->column_int64(0));
  }
  return ids;
}

auto sweep(db::connection& conn, std::int64_t since_days, std::optional<std::string_view> scope_slug)
    -> std::expected<std::size_t, annotation_error> {
  auto ids = sweep_candidates(conn, since_days, scope_slug);
  if (!ids) {
    return std::unexpected(ids.error());
  }
  std::size_t count = 0;
  for (const auto id : *ids) {
    auto archived = archive(conn, id);
    if (!archived) {
      if (archived.error() == annotation_error::terminal_status) {
        continue;
      }
      return std::unexpected(archived.error());
    }
    ++count;
  }
  return count;
}

auto render_sweep_json(std::int64_t since_days, std::size_t swept) -> std::string {
  return std::format(R"({{"ok":true,"action":"sweep","since_days":{},"swept":{}}})", since_days, swept);
}

auto render_sweep_text(std::int64_t since_days, std::size_t swept) -> std::string {
  return std::format("sweep: archived {} annotation(s) older than {} day(s)", swept, since_days);
}

// ---------------------------------------------------------------------------
// verify
// ---------------------------------------------------------------------------

auto classify_anchor(std::string_view stored_text_hash, std::optional<std::string_view> file_contents) -> verify_state {
  if (!file_contents.has_value()) {
    return verify_state::stale;
  }
  if (stored_text_hash.empty()) {
    return verify_state::fresh;
  }
  const auto digest = sha256_hex(*file_contents);
  // The Zig comparison takes the first `min(stored.len, 64)` characters of
  // the computed hex and compares against the WHOLE stored value -- so a
  // stored PREFIX matches, and a stored value longer than 64 characters
  // can never match.
  const auto computed = std::string_view{digest}.substr(0, std::min<std::size_t>(stored_text_hash.size(), 64));
  return stored_text_hash == computed ? verify_state::fresh : verify_state::drifted;
}

auto verify_state_to_text(verify_state s) -> std::string_view {
  switch (s) {
  case verify_state::fresh:
    return "fresh";
  case verify_state::drifted:
    return "drifted";
  case verify_state::stale:
    return "stale";
  }
  return "stale"; // unreachable
}

auto render_verify_json(const std::vector<verify_row>& rows) -> std::string {
  std::string out = R"({"ok":true,"rows":[)";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += std::format(R"({{"id":{},"anchor_path":{},"state":"{}"}})", rows[i].id, json_string(rows[i].anchor_path),
                       verify_state_to_text(rows[i].state));
  }
  out += "]}";
  return out;
}

auto render_verify_text(const std::vector<verify_row>& rows) -> std::string {
  if (rows.empty()) {
    return "(no active annotations)\n";
  }
  std::string out;
  for (const auto& r : rows) {
    out += std::format("annotation:{}  {}  [{}]\n", r.id, r.anchor_path, verify_state_to_text(r.state));
  }
  return out;
}

// ---------------------------------------------------------------------------
// show / list rendering
// ---------------------------------------------------------------------------

auto render_json(const annotation& a) -> std::string {
  std::string out;
  out += std::format(R"({{"id":{},"scope_kind":"{}","scope_id":{},)", a.id, scope_kind_to_text(a.scope_kind_),
                     json_optional_int(a.scope_id));
  out += std::format(R"("anchor":{{"path":{},"line_start":{},"line_end":{},)", json_string(a.anchor.path),
                     json_optional_int(a.anchor.line_start), json_optional_int(a.anchor.line_end));
  out += std::format(R"("commit_sha":{},"text_hash":{},"text":{}}},)", json_string(a.anchor.commit_sha),
                     json_string(a.anchor.text_hash), json_string(a.anchor.text));
  out += std::format(R"("title":{},"slug":{},"body":{},"status":"{}","vendor":{},)", json_optional_string(a.title),
                     json_optional_string(a.slug), json_string(a.body), status_to_text(a.status_), json_string(a.vendor));
  out += std::format(R"("plan_id":{},"task_id":{},"tags":[)", json_optional_int(a.plan_id), json_optional_int(a.task_id));
  for (std::size_t i = 0; i < a.tags.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += json_string(a.tags[i]);
  }
  out += std::format(R"(],"created_at":{},"updated_at":{}}})", json_string(a.created_at), json_string(a.updated_at));
  return out;
}

auto render_list_json(const std::vector<annotation>& items) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out += ",";
    }
    out += render_json(items[i]);
  }
  out += "]";
  return out;
}

auto render_text(const annotation& a) -> std::string {
  std::string out;
  out += std::format("id:          {}\n", a.id);
  if (a.title.has_value()) {
    out += std::format("title:       {}\n", *a.title);
  }
  if (a.slug.has_value()) {
    out += std::format("slug:        {}\n", *a.slug);
  }
  out += std::format("status:      {}\n", status_to_text(a.status_));
  out += std::format("scope:       {}", scope_kind_to_text(a.scope_kind_));
  if (a.scope_id.has_value()) {
    out += std::format(":{}", *a.scope_id);
  }
  out += "\n";
  out += std::format("anchor path: {}\n", a.anchor.path);
  if (a.anchor.line_start.has_value()) {
    if (a.anchor.line_end.has_value()) {
      out += std::format("anchor line: {}-{}\n", *a.anchor.line_start, *a.anchor.line_end);
    } else {
      out += std::format("anchor line: {}\n", *a.anchor.line_start);
    }
  }
  if (!a.anchor.commit_sha.empty()) {
    out += std::format("anchor sha:  {}\n", a.anchor.commit_sha);
  }
  if (!a.anchor.text_hash.empty()) {
    out += std::format("anchor hash: {}\n", a.anchor.text_hash);
  }
  if (!a.vendor.empty()) {
    out += std::format("vendor:      {}\n", a.vendor);
  }
  if (a.plan_id.has_value()) {
    out += std::format("plan:        {}\n", *a.plan_id);
  }
  if (a.task_id.has_value()) {
    out += std::format("task:        {}\n", *a.task_id);
  }
  if (!a.tags.empty()) {
    out += "tags:        ";
    for (std::size_t i = 0; i < a.tags.size(); ++i) {
      if (i > 0) {
        out += ", ";
      }
      out += a.tags[i];
    }
    out += "\n";
  }
  if (!a.body.empty()) {
    out += std::format("body:        {}\n", a.body);
  }
  out += std::format("created:     {}\n", a.created_at);
  out += std::format("updated:     {}\n", a.updated_at);
  return out;
}

auto render_list_text(const std::vector<annotation>& items) -> std::string {
  if (items.empty()) {
    return "(no annotations)\n";
  }
  std::string out;
  for (const auto& a : items) {
    // Title falls back to the anchor path when the annotation has none.
    const std::string& title = a.title.has_value() ? *a.title : a.anchor.path;
    // zig `{d:>5}  {s:<10}  {s}` over (id, status, title): right-aligned
    // width 5, two spaces, left-aligned width 10, two spaces, title.
    // Oracle: `    1  active      A1`.
    out += std::format("{:>5}  {:<10}  {}\n", a.id, status_to_text(a.status_), title);
  }
  return out;
}

auto render_tag_json(std::int64_t id, std::string_view tag, bool removing) -> std::string {
  return std::format(R"({{"ok":true,"id":{},"tag":{},"action":"{}"}})", id, json_string(tag), removing ? "remove" : "add");
}

auto render_tag_text(std::int64_t id, std::string_view tag, bool removing) -> std::string {
  return std::format("annotation {}: {} tag '{}'", id, removing ? "removed" : "added", tag);
}

auto render_remove_json(std::int64_t id) -> std::string {
  return std::format(R"({{"ok":true,"id":{}}})", id);
}

auto render_remove_text(std::int64_t id) -> std::string {
  return std::format("annotation {} removed", id);
}

} // namespace planar::engine::planning::annotation

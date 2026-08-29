/// @file materialize.cpp
/// @brief Implementation of `planar.engine.ingest.materialize` (see materialize.cppm).
///
/// Behavior-preserving port (D2) of `zig/src/engine/ingestor/materialize.zig`.
/// The SQL, the staging order, the digest's canonical form, and the Markdown
/// section-resolution rules are all carried over unchanged — each of them is
/// either a stored contract or a freshness contract that a "cleanup" would
/// break silently rather than loudly.

module planar.engine.ingest.materialize;

import std;
import planar.db;
import planar.engine.ingest.parse;

namespace planar::engine::ingest::materialize {
namespace {

constexpr std::string_view whitespace_inline = " \t";
constexpr std::string_view whitespace_block  = " \t\r\n";

[[nodiscard]] auto trim(std::string_view s, std::string_view chars) -> std::string_view {
  const auto first = s.find_first_not_of(chars);
  if (first == std::string_view::npos) {
    return {};
  }
  return s.substr(first, s.find_last_not_of(chars) - first + 1);
}

[[nodiscard]] auto trim_end(std::string_view s, std::string_view chars) -> std::string_view {
  const auto last = s.find_last_not_of(chars);
  return last == std::string_view::npos ? std::string_view{} : s.substr(0, last + 1);
}

/// @brief ASCII case-insensitive substring search.
[[nodiscard]] auto contains_fold(std::string_view haystack, std::string_view needle) -> bool {
  if (needle.empty() || needle.size() > haystack.size()) {
    return false;
  }
  const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; };
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (std::ranges::equal(haystack.substr(i, needle.size()), needle, [&](char a, char b) { return lower(a) == lower(b); })) {
      return true;
    }
  }
  return false;
}

/// @brief ASCII case-insensitive equality.
[[nodiscard]] auto equals_fold(std::string_view a, std::string_view b) -> bool {
  if (a.size() != b.size()) {
    return false;
  }
  const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c; };
  return std::ranges::equal(a, b, [&](char x, char y) { return lower(x) == lower(y); });
}

// --- SHA-256 ---------------------------------------------------------------
//
// Implemented here rather than pulled in as a dependency: this is the only
// consumer in the tree, and the project's vendoring rule (pinned release
// archive plus SHA256 in cmake/dependencies.cmake) is a real cost to pay for
// ~80 lines of a fully specified, test-vector-verifiable algorithm. If a
// second bucket ever needs digests, D19 says extract it to a layer-1 module at
// that point — one consumer does not trigger the rule.
//
// FIPS 180-4. Verified against the standard test vectors in materialize.t.cpp.

constexpr std::array<std::uint32_t, 64> sha256_k = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

[[nodiscard]] constexpr auto rotr(std::uint32_t x, unsigned n) -> std::uint32_t {
  return (x >> n) | (x << (32U - n));
}

/// The algorithm itself. `sha256_hex` below is the exported thin wrapper
/// (task 6324) — kept separate so the module-scope name and this
/// internal-linkage one cannot collide in unqualified lookup.
[[nodiscard]] auto sha256_hex_raw(std::string_view input) -> std::string {
  std::array<std::uint32_t, 8> h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  // Message + 0x80 + zero padding to 56 mod 64 + 64-bit big-endian bit length.
  std::vector<std::uint8_t> msg(input.size());
  std::ranges::transform(input, msg.begin(), [](char c) { return static_cast<std::uint8_t>(c); });
  const std::uint64_t bit_len = static_cast<std::uint64_t>(input.size()) * 8U;
  msg.push_back(0x80U);
  while (msg.size() % 64U != 56U) {
    msg.push_back(0x00U);
  }
  for (int i = 7; i >= 0; --i) {
    msg.push_back(static_cast<std::uint8_t>((bit_len >> (static_cast<unsigned>(i) * 8U)) & 0xFFU));
  }

  std::array<std::uint32_t, 64> w{};
  for (std::size_t chunk = 0; chunk < msg.size(); chunk += 64U) {
    for (std::size_t i = 0; i < 16U; ++i) {
      w[i] = (static_cast<std::uint32_t>(msg[chunk + (i * 4U) + 0]) << 24U) |
             (static_cast<std::uint32_t>(msg[chunk + (i * 4U) + 1]) << 16U) |
             (static_cast<std::uint32_t>(msg[chunk + (i * 4U) + 2]) << 8U) |
             (static_cast<std::uint32_t>(msg[chunk + (i * 4U) + 3]));
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
      const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
      const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
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
      const auto s1    = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const auto ch    = (e & f) ^ (~e & g);
      const auto temp1 = hh + s1 + ch + sha256_k[i] + w[i];
      const auto s0    = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const auto maj   = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + maj;

      hh = g;
      g  = f;
      f  = e;
      e  = d + temp1;
      d  = c;
      c  = b;
      b  = a;
      a  = temp1 + temp2;
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
    out.append(std::format("{:08x}", word));
  }
  return out;
}

// --- Markdown heading / fence recognition ----------------------------------

/// @brief One ATX heading line.
struct heading {
  std::size_t      level_ = 0;
  std::string_view title_;
};

/// @brief Recognises an ATX heading (`#` … `######`), stripping closing hashes.
///
/// Up to three leading spaces are allowed (four makes it indented code).
[[nodiscard]] auto atx_heading(std::string_view line) -> std::optional<heading> {
  const auto  without_cr = trim_end(line, "\r");
  std::size_t indent     = 0;
  while (indent < without_cr.size() && without_cr[indent] == ' ' && indent < 4) {
    ++indent;
  }
  if (indent > 3 || indent >= without_cr.size() || without_cr[indent] != '#') {
    return std::nullopt;
  }
  std::size_t level = 0;
  while (indent + level < without_cr.size() && without_cr[indent + level] == '#' && level < 7) {
    ++level;
  }
  if (level == 0 || level > 6) {
    return std::nullopt;
  }
  const auto after = indent + level;
  if (after < without_cr.size() && without_cr[after] != ' ' && without_cr[after] != '\t') {
    return std::nullopt;
  }
  auto title = trim(without_cr.substr(after), whitespace_inline);
  while (!title.empty() && title.back() == '#') {
    title = trim_end(title.substr(0, title.size() - 1), whitespace_inline);
  }
  return heading{.level_ = level, .title_ = title};
}

/// @brief One fenced-code-block delimiter line.
struct fence {
  char        char_       = '\0';
  std::size_t len_        = 0;
  bool        is_closing_ = false; ///< Nothing but whitespace follows the run.
};

[[nodiscard]] auto fence_delimiter(std::string_view line) -> std::optional<fence> {
  const auto  without_cr = trim_end(line, "\r");
  std::size_t indent     = 0;
  while (indent < without_cr.size() && without_cr[indent] == ' ' && indent < 4) {
    ++indent;
  }
  if (indent > 3 || indent >= without_cr.size()) {
    return std::nullopt;
  }
  const char c = without_cr[indent];
  if (c != '`' && c != '~') {
    return std::nullopt;
  }
  std::size_t count = 0;
  while (indent + count < without_cr.size() && without_cr[indent + count] == c) {
    ++count;
  }
  if (count < 3) {
    return std::nullopt;
  }
  return fence{.char_ = c, .len_ = count, .is_closing_ = trim(without_cr.substr(indent + count), whitespace_inline).empty()};
}

[[nodiscard]] auto is_indented_code(std::string_view line) -> bool {
  if (line.empty()) {
    return false;
  }
  if (line.front() == '\t') {
    return true;
  }
  std::size_t spaces = 0;
  while (spaces < line.size() && line[spaces] == ' ') {
    ++spaces;
  }
  return spaces >= 4;
}

// --- Fact staging ----------------------------------------------------------

/// @brief A staged fact's typed value.
using fact_value = std::variant<bool, std::int64_t, std::string_view>;

/// @brief One `(needle → fact kind)` evidence rule.
struct evidence_flag {
  std::string_view needle_;
  std::string_view fact_kind_;
};

/// @brief The evidence rules, in the original's order.
///
/// Order is not cosmetic: it is the order rows enter the staging table, and
/// the staged set is compared against the stored set as a whole.
constexpr std::array<evidence_flag, 12> evidence_flags = {{
    {.needle_ = "schema", .fact_kind_ = "risk_schema"},
    {.needle_ = "migration", .fact_kind_ = "risk_schema"},
    {.needle_ = "transaction", .fact_kind_ = "risk_transaction"},
    {.needle_ = "atomic", .fact_kind_ = "risk_transaction"},
    {.needle_ = "concurren", .fact_kind_ = "risk_concurrency"},
    {.needle_ = "ownership", .fact_kind_ = "risk_ownership"},
    {.needle_ = "security", .fact_kind_ = "risk_security"},
    {.needle_ = "state transition", .fact_kind_ = "risk_state_transition"},
    {.needle_ = "resource lifecycle", .fact_kind_ = "risk_resource_lifecycle"},
    {.needle_ = "architecture", .fact_kind_ = "risk_architecture"},
    {.needle_ = "mechanical", .fact_kind_ = "mechanicality_evidence"},
    {.needle_ = "enumerated", .fact_kind_ = "mechanicality_evidence"},
}};

[[nodiscard]] auto query_failure() -> std::unexpected<materialize_error> {
  return std::unexpected(materialize_error{.kind_ = materialize_error_kind::query_failed, .citations_ = {}});
}

/// @brief Inserts one fact into the staging table.
[[nodiscard]] auto stage(db::connection& conn, std::int64_t task_id, std::string_view fact_kind, const fact_value& value,
                         std::string_view source_kind, std::int64_t source_id, std::string_view locator,
                         std::string_view semantic_source) -> std::expected<void, materialize_error> {
  const auto digest = source_digest(source_kind, source_id, locator, semantic_source);

  auto stmt = conn.prepare(R"(insert into temp.routing_task_facts_stage (
  task_id, fact_kind, value_type, value_bool, value_integer,
  value_text, source_entity_kind, source_entity_id, source_locator,
  source_digest, materializer_version
) values (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?))");
  if (!stmt.has_value()) {
    return query_failure();
  }

  const std::string_view value_type = std::visit(
      []<typename T>(const T&) -> std::string_view {
        if constexpr (std::is_same_v<T, bool>) {
          return "bool";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
          return "integer";
        } else {
          return "text";
        }
      },
      value);

  const bool ok =
      stmt->bind_int64(1, task_id).has_value() && stmt->bind_text(2, fact_kind).has_value() &&
      stmt->bind_text(3, value_type).has_value() &&
      (std::holds_alternative<bool>(value) ? stmt->bind_int64(4, std::get<bool>(value) ? 1 : 0).has_value()
                                           : stmt->bind_null(4).has_value()) &&
      (std::holds_alternative<std::int64_t>(value) ? stmt->bind_int64(5, std::get<std::int64_t>(value)).has_value()
                                                   : stmt->bind_null(5).has_value()) &&
      (std::holds_alternative<std::string_view>(value) ? stmt->bind_text(6, std::get<std::string_view>(value)).has_value()
                                                       : stmt->bind_null(6).has_value()) &&
      stmt->bind_text(7, source_kind).has_value() && stmt->bind_int64(8, source_id).has_value() &&
      stmt->bind_text(9, locator).has_value() && stmt->bind_text(10, digest).has_value() &&
      stmt->bind_text(11, materializer_version).has_value();
  if (!ok) {
    return query_failure();
  }
  if (!stmt->step().has_value()) {
    return query_failure();
  }
  return {};
}

/// @brief Stages every evidence flag whose needle appears in `text`.
[[nodiscard]] auto stage_evidence_flags(db::connection& conn, std::int64_t task_id, std::string_view source_kind,
                                        std::int64_t source_id, std::string_view locator, std::string_view text)
    -> std::expected<void, materialize_error> {
  for (const auto& flag : evidence_flags) {
    if (contains_fold(text, flag.needle_)) {
      auto staged = stage(conn, task_id, flag.fact_kind_, fact_value{true}, source_kind, source_id, locator, text);
      if (!staged.has_value()) {
        return staged;
      }
    }
  }
  return {};
}

[[nodiscard]] auto create_stage(db::connection& conn) -> std::expected<void, materialize_error> {
  const auto created = conn.execute(R"(create temp table if not exists routing_task_facts_stage (
  task_id integer not null,
  fact_kind text not null,
  value_type text not null,
  value_bool integer,
  value_integer integer,
  value_text text,
  source_entity_kind text not null,
  source_entity_id integer not null,
  source_locator text not null,
  source_digest text not null,
  materializer_version text not null
))");
  if (!created.has_value()) {
    return query_failure();
  }
  return {};
}

[[nodiscard]] auto stage_task_facts(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<void, materialize_error> {
  auto stmt = conn.prepare(R"(select t.id, coalesce(t.body, ''), coalesce(t.next_action, '')
from tasks t join plans p on p.id = t.plan_id
where p.parent_plan_id = ?
order by t.id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, anchor_plan_id).has_value()) {
    return query_failure();
  }
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return query_failure();
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    const auto task_id     = stmt->column_int64(0);
    const auto body        = stmt->column_text(1);
    const auto next_action = stmt->column_text(2);

    const auto acceptance = section(body, "## Acceptance Criteria");
    // The generic ingest placeholder is explicitly UNREADY: routing rejects
    // that phrase, and Planar must not certify text Planar itself generated.
    const bool acceptance_ready = !acceptance.empty() && !contains_fold(acceptance, "is implemented and tested.");
    if (auto r = stage(conn, task_id, "acceptance_complete", fact_value{acceptance_ready}, "task", task_id,
                       "body#acceptance-criteria", acceptance);
        !r.has_value()) {
      return r;
    }
    if (!acceptance.empty()) {
      if (auto r = stage(conn, task_id, "acceptance_text", fact_value{acceptance}, "task", task_id, "body#acceptance-criteria",
                         acceptance);
          !r.has_value()) {
        return r;
      }
    }

    const bool next_ready = !next_action.empty() && !contains_fold(next_action, "implement per acceptance criteria");
    if (auto r = stage(conn, task_id, "next_action_exact", fact_value{next_ready}, "task", task_id, "next_action", next_action);
        !r.has_value()) {
      return r;
    }
    if (!next_action.empty()) {
      if (auto r = stage(conn, task_id, "next_action", fact_value{std::string_view{next_action}}, "task", task_id, "next_action",
                         next_action);
          !r.has_value()) {
        return r;
      }
    }

    if (auto r = stage_evidence_flags(conn, task_id, "task", task_id, "body", body); !r.has_value()) {
      return r;
    }
    if (!body.empty()) {
      if (auto r = stage(conn, task_id, "context_bytes", fact_value{static_cast<std::int64_t>(body.size())}, "task", task_id,
                         "body", body);
          !r.has_value()) {
        return r;
      }
    }
  }
  return {};
}

[[nodiscard]] auto stage_roadmap_facts(db::connection& conn, std::span<const roadmap_citation> citations)
    -> std::expected<void, materialize_error> {
  for (const auto& citation : citations) {
    if (auto r = stage(conn, citation.task_id_, "cited_artifact_section", fact_value{std::string_view{citation.source_text_}},
                       "artifact", citation.artifact_id_, citation.source_locator_, citation.source_text_);
        !r.has_value()) {
      return r;
    }
  }
  return {};
}

[[nodiscard]] auto has_parsed_roadmap_citation(std::span<const roadmap_citation> citations, std::int64_t task_id,
                                               std::int64_t artifact_id) -> bool {
  return std::ranges::any_of(citations,
                             [&](const roadmap_citation& c) { return c.task_id_ == task_id && c.artifact_id_ == artifact_id; });
}

/// @brief Extracts the citation locator for `artifact_id` out of a task body.
///
/// The scan runs from the `artifact:<id>#` marker to end-of-line, stopping
/// early at `,`, `)` or `]`. That termination rule is ported UNCHANGED — it
/// decides which citations resolve, so relaxing it here would change behavior
/// rather than preserve it. Its consequence (a heading containing any of those
/// characters can never be cited in full) is what the diagnostic's
/// `truncated_from_` field exists to name.
/// @return `nullopt` when the marker is absent or the locator is bare.
[[nodiscard]] auto explicit_artifact_locator(std::string_view task_body, std::int64_t artifact_id) -> std::optional<std::string> {
  const auto marker = std::format("artifact:{}#", artifact_id);
  const auto start  = task_body.find(marker);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const auto  tail = task_body.substr(start);
  std::size_t end  = 0;
  while (end < tail.size() && tail[end] != '\n' && tail[end] != '\r' && tail[end] != ',' && tail[end] != ')' &&
         tail[end] != ']') {
    ++end;
  }
  const auto locator = trim(tail.substr(0, end), whitespace_inline);
  if (locator.size() <= marker.size()) {
    return std::nullopt;
  }
  return std::string{locator};
}

[[nodiscard]] auto has_explicit_artifact_reference(std::string_view task_body, std::int64_t artifact_id) -> bool {
  return task_body.find(std::format("artifact:{}#", artifact_id)) != std::string_view::npos;
}

/// @brief Builds the diagnostic for one unresolvable citation.
///
/// Collects every authored `## ` heading the artifact offers, skipping the
/// synthetic `## Content` wrapper (which is not authored content and must not
/// be offered as something to cite), then looks for the truncation signature:
/// an offered section that STARTS WITH the requested name. That match is
/// near-conclusive evidence the locator was cut short at a `,`, `)` or `]`
/// inside the heading, which is the single most common cause of this failure
/// and the one an operator is least likely to guess.
[[nodiscard]] auto build_citation_diagnostic(std::int64_t task_id, std::int64_t artifact_id, std::string_view locator,
                                             std::string_view body) -> citation_diagnostic {
  const auto hash   = locator.find('#');
  const auto wanted = hash == std::string_view::npos ? locator : locator.substr(hash + 1);

  citation_diagnostic diagnostic{.task_id_        = task_id,
                                 .artifact_id_    = artifact_id,
                                 .locator_        = std::string{locator},
                                 .wanted_         = std::string{wanted},
                                 .available_      = {},
                                 .truncated_from_ = {}};

  std::size_t offset = 0;
  while (offset < body.size()) {
    const auto line_end = body.find('\n', offset);
    const auto line     = body.substr(offset, line_end == std::string_view::npos ? std::string_view::npos : line_end - offset);
    if (line.starts_with("## ")) {
      if (const auto name = trim(line.substr(3), " \t\r"); name != "Content") {
        diagnostic.available_.emplace_back(name);
      }
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    offset = line_end + 1;
  }

  if (!wanted.empty()) {
    for (const auto& available : diagnostic.available_) {
      if (available.size() > wanted.size() && std::string_view{available}.starts_with(wanted)) {
        diagnostic.truncated_from_ = available;
        break;
      }
    }
  }
  return diagnostic;
}

[[nodiscard]] auto stage_artifact_facts(db::connection& conn, std::int64_t anchor_plan_id,
                                        std::span<const roadmap_citation> roadmap_citations)
    -> std::expected<void, materialize_error> {
  std::vector<citation_diagnostic> failures;

  auto stmt = conn.prepare(R"(select t.id, coalesce(t.body, ''), a.id, a.kind, coalesce(a.body, '')
from tasks t
join plans p on p.id = t.plan_id
join entity_links el on el.from_kind = 'task' and el.from_id = t.id
  and el.to_kind = 'artifact' and el.relationship = 'cites'
join artifacts a on a.id = el.to_id
where p.parent_plan_id = ?
order by t.id, a.id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, anchor_plan_id).has_value()) {
    return query_failure();
  }

  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return query_failure();
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    const auto task_id     = stmt->column_int64(0);
    const auto task_body   = stmt->column_text(1);
    const auto artifact_id = stmt->column_int64(2);
    const auto kind        = stmt->column_text(3);
    const auto body        = stmt->column_text(4);

    if (kind == "roadmap") {
      // A roadmap citation the parser already produced is staged from the
      // parsed provenance, not re-derived from the body.
      if (has_parsed_roadmap_citation(roadmap_citations, task_id, artifact_id)) {
        continue;
      }
      if (!has_explicit_artifact_reference(task_body, artifact_id)) {
        continue;
      }
    }

    const auto locator = explicit_artifact_locator(task_body, artifact_id);
    if (!locator.has_value()) {
      // No usable locator at all. The original returns a bare error here; name
      // it the same way every other citation failure is named.
      failures.push_back(build_citation_diagnostic(task_id, artifact_id, std::format("artifact:{}#", artifact_id), body));
      continue;
    }
    const auto cited_source = artifact_section(body, *locator);
    if (!cited_source.has_value()) {
      failures.push_back(build_citation_diagnostic(task_id, artifact_id, *locator, body));
      // Keep scanning so EVERY bad citation is reported in one pass; the whole
      // reconcile still fails below, so nothing is staged from a
      // partially-resolved citation set.
      continue;
    }
    if (auto r = stage(conn, task_id, "cited_artifact_section", fact_value{*cited_source}, "artifact", artifact_id, *locator,
                       *cited_source);
        !r.has_value()) {
      return r;
    }
  }

  if (!failures.empty()) {
    return std::unexpected(
        materialize_error{.kind_ = materialize_error_kind::invalid_citation, .citations_ = std::move(failures)});
  }
  return {};
}

[[nodiscard]] auto stage_decision_facts(db::connection& conn, std::int64_t anchor_plan_id)
    -> std::expected<void, materialize_error> {
  auto stmt = conn.prepare(R"(select t.id, de.id, de.body
from tasks t
join plans p on p.id = t.plan_id
join entity_links el on el.from_kind = 'decision'
  and el.to_kind = 'plan' and el.to_id = ?
  and el.relationship = 'derives-from'
join decisions de on de.id = el.from_id and de.status = 'accepted'
where p.parent_plan_id = ?
order by t.id, de.id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, anchor_plan_id).has_value() || !stmt->bind_int64(2, anchor_plan_id).has_value()) {
    return query_failure();
  }
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return query_failure();
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    const auto task_id     = stmt->column_int64(0);
    const auto decision_id = stmt->column_int64(1);
    const auto body        = stmt->column_text(2);
    if (auto r =
            stage(conn, task_id, "locked_decision", fact_value{std::string_view{body}}, "decision", decision_id, "body", body);
        !r.has_value()) {
      return r;
    }
    if (auto r = stage_evidence_flags(conn, task_id, "decision", decision_id, "body", body); !r.has_value()) {
      return r;
    }
  }
  return {};
}

[[nodiscard]] auto stage_question_facts(db::connection& conn, std::int64_t anchor_plan_id)
    -> std::expected<void, materialize_error> {
  auto stmt = conn.prepare(R"(select t.id, q.id, coalesce(q.body, '')
from tasks t
join plans p on p.id = t.plan_id
join entity_links el on el.from_kind = 'question'
  and el.to_kind = 'plan' and el.to_id = ?
  and el.relationship = 'derives-from'
join questions q on q.id = el.from_id and q.status = 'open'
where p.parent_plan_id = ?
order by t.id, q.id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, anchor_plan_id).has_value() || !stmt->bind_int64(2, anchor_plan_id).has_value()) {
    return query_failure();
  }
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return query_failure();
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    const auto task_id     = stmt->column_int64(0);
    const auto question_id = stmt->column_int64(1);
    const auto body        = stmt->column_text(2);
    if (auto r = stage(conn, task_id, "unresolved_question", fact_value{true}, "question", question_id, "status", body);
        !r.has_value()) {
      return r;
    }
  }
  return {};
}

[[nodiscard]] auto stage_scenario_facts(db::connection& conn, std::int64_t anchor_plan_id)
    -> std::expected<void, materialize_error> {
  auto stmt = conn.prepare(R"(select t.id, s.id, coalesce(s.body, ''), s.status
from tasks t
join plans p on p.id = t.plan_id
join entity_links el on el.to_kind = 'task' and el.to_id = t.id
  and el.from_kind = 'test_scenario' and el.relationship = 'verifies'
join test_scenarios s on s.id = el.from_id
where p.parent_plan_id = ?
order by t.id, s.id)");
  if (!stmt.has_value() || !stmt->bind_int64(1, anchor_plan_id).has_value()) {
    return query_failure();
  }
  while (true) {
    const auto stepped = stmt->step();
    if (!stepped.has_value()) {
      return query_failure();
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    const auto task_id     = stmt->column_int64(0);
    const auto scenario_id = stmt->column_int64(1);
    const auto body        = stmt->column_text(2);
    const auto status      = stmt->column_text(3);
    if (auto r = stage(conn, task_id, "scenario", fact_value{std::string_view{body}}, "test_scenario", scenario_id, "body", body);
        !r.has_value()) {
      return r;
    }
    if (auto r = stage(conn, task_id, "scenario_coverage", fact_value{std::string_view{status}}, "test_scenario", scenario_id,
                       "status", status);
        !r.has_value()) {
      return r;
    }
    if (const auto gate = field(body, "**Acceptance:**"); gate.has_value()) {
      if (auto r =
              stage(conn, task_id, "validation_gate", fact_value{*gate}, "test_scenario", scenario_id, "body#acceptance", *gate);
          !r.has_value()) {
        return r;
      }
    }
    if (auto r = stage_evidence_flags(conn, task_id, "test_scenario", scenario_id, "body", body); !r.has_value()) {
      return r;
    }
  }
  return {};
}

/// @brief Stages one aggregate count fact per task.
///
/// The semantic source is `"<counted_kind>:<count>"` — the exact string the
/// routing packet recomputes to decide freshness (see the constants' doc
/// comment in materialize.cppm).
[[nodiscard]] auto stage_count(db::connection& conn, std::int64_t anchor_plan_id, std::string_view counted_kind,
                               std::string_view fact_kind, std::string_view locator) -> std::expected<void, materialize_error> {
  // Collected first, then staged: the query reads the staging table, and
  // inserting into it while the cursor is open would be reading and writing
  // the same table in one statement.
  std::vector<std::pair<std::int64_t, std::int64_t>> counts;
  {
    auto stmt = conn.prepare(R"(select t.id, count(s.fact_kind)
from tasks t join plans p on p.id = t.plan_id
left join temp.routing_task_facts_stage s
  on s.task_id = t.id and s.fact_kind = ?
where p.parent_plan_id = ?
group by t.id order by t.id)");
    if (!stmt.has_value() || !stmt->bind_text(1, counted_kind).has_value() || !stmt->bind_int64(2, anchor_plan_id).has_value()) {
      return query_failure();
    }
    while (true) {
      const auto stepped = stmt->step();
      if (!stepped.has_value()) {
        return query_failure();
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      counts.emplace_back(stmt->column_int64(0), stmt->column_int64(1));
    }
  }

  for (const auto& [task_id, count] : counts) {
    if (count <= 0) {
      continue;
    }
    const auto semantic_count = std::format("{}:{}", counted_kind, count);
    if (auto r = stage(conn, task_id, fact_kind, fact_value{count}, "task", task_id, locator, semantic_count); !r.has_value()) {
      return r;
    }
  }
  return {};
}

[[nodiscard]] auto stage_link_facts(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<void, materialize_error> {
  struct link_row {
    std::int64_t task_id_ = 0;
    std::string  relationship_;
    std::string  from_kind_;
    std::int64_t from_id_ = 0;
    std::string  to_kind_;
    std::int64_t to_id_ = 0;
  };
  std::vector<link_row> rows;
  {
    auto stmt = conn.prepare(R"(select t.id, el.relationship, el.from_kind, el.from_id,
       el.to_kind, el.to_id
from tasks t
join plans p on p.id = t.plan_id
join entity_links el on (
  (el.from_kind = 'task' and el.from_id = t.id)
  or (el.to_kind = 'task' and el.to_id = t.id)
)
where p.parent_plan_id = ?
  and el.relationship in ('touches', 'depends-on')
order by t.id, el.from_kind, el.from_id, el.to_kind, el.to_id)");
    if (!stmt.has_value() || !stmt->bind_int64(1, anchor_plan_id).has_value()) {
      return query_failure();
    }
    while (true) {
      const auto stepped = stmt->step();
      if (!stepped.has_value()) {
        return query_failure();
      }
      if (*stepped == db::step_result::done) {
        break;
      }
      rows.push_back({.task_id_      = stmt->column_int64(0),
                      .relationship_ = stmt->column_text(1),
                      .from_kind_    = stmt->column_text(2),
                      .from_id_      = stmt->column_int64(3),
                      .to_kind_      = stmt->column_text(4),
                      .to_id_        = stmt->column_int64(5)});
    }
  }

  for (const auto& row : rows) {
    const bool             task_is_source = row.from_kind_ == "task" && row.from_id_ == row.task_id_;
    const auto&            other_kind     = task_is_source ? row.to_kind_ : row.from_kind_;
    const auto             other_id       = task_is_source ? row.to_id_ : row.from_id_;
    const auto             locator        = std::format("{}:{}", other_kind, other_id);
    const std::string_view fact_kind      = row.relationship_ == "touches" ? "touch" : (task_is_source ? "blocks" : "blocked_by");
    if (auto r = stage(conn, row.task_id_, fact_kind, fact_value{other_id}, other_kind, other_id, locator, row.relationship_);
        !r.has_value()) {
      return r;
    }
  }

  if (auto r = stage_count(conn, anchor_plan_id, counted_kind_touch, "breadth", locator_touches); !r.has_value()) {
    return r;
  }
  if (auto r = stage_count(conn, anchor_plan_id, counted_kind_scenario, "validation_burden", locator_scenarios); !r.has_value()) {
    return r;
  }
  return stage_count(conn, anchor_plan_id, counted_kind_dependency, "dependency_fanout", locator_dependency_fanout);
}

/// @brief Whether the staged set and the stored set are semantically identical.
///
/// A symmetric EXCEPT on both sides: equal sets mean the rewrite is skipped
/// entirely, which is what keeps stored fact ids stable across a replay.
[[nodiscard]] auto fact_sets_equal(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<bool, materialize_error> {
  auto stmt = conn.prepare(R"(with current_facts as (
  select task_id, fact_kind, value_type, value_bool, value_integer,
         value_text, source_entity_kind, source_entity_id,
         source_locator, source_digest, materializer_version
  from routing_task_facts
  where task_id in (
    select t.id from tasks t
    join plans p on p.id = t.plan_id
    where p.parent_plan_id = ?
  )
),
delta as (
  select * from (
    select * from current_facts
    except select * from temp.routing_task_facts_stage
  )
  union all
  select * from (
    select * from temp.routing_task_facts_stage
    except select * from current_facts
  )
)
select count(*) from delta)");
  if (!stmt.has_value() || !stmt->bind_int64(1, anchor_plan_id).has_value()) {
    return query_failure();
  }
  const auto stepped = stmt->step();
  if (!stepped.has_value()) {
    return query_failure();
  }
  if (*stepped == db::step_result::done) {
    return false;
  }
  return stmt->column_int64(0) == 0;
}

} // namespace

auto citation_diagnostic::describe() const -> std::string {
  std::string out = std::format(R"(task {} cites artifact {} section "{}" (locator "{}"), which that artifact does not contain)",
                                task_id_, artifact_id_, wanted_, locator_);
  if (available_.empty()) {
    out.append("; that artifact offers no citable sections at all");
  } else {
    out.append("; it offers: ");
    for (std::size_t i = 0; i < available_.size(); ++i) {
      if (i > 0) {
        out.append(", ");
      }
      out.append(available_[i]);
    }
  }
  if (!truncated_from_.empty()) {
    out.append(std::format(
        R"(. The locator looks TRUNCATED: section "{}" starts with the requested name, and a locator stops at the first ',', ')' or ']'. Rename that heading to drop the character, or cite a heading without one)",
        truncated_from_));
  }
  return out;
}

auto materialize_error::describe() const -> std::string {
  if (kind_ == materialize_error_kind::query_failed) {
    return "materializing routing facts: a database query failed";
  }
  std::string out = std::format("materializing routing facts: {} unresolvable citation(s)", citations_.size());
  for (const auto& diagnostic : citations_) {
    out.append("\n  ");
    out.append(diagnostic.describe());
  }
  return out;
}

auto sha256_hex(std::string_view input) -> std::string {
  return sha256_hex_raw(input);
}

auto source_digest(std::string_view source_kind, std::int64_t source_id, std::string_view locator,
                   std::string_view semantic_source) -> std::string {
  std::string canonical;
  canonical.append(source_kind);
  canonical.push_back('\0');
  canonical.append(std::format("{}", source_id));
  canonical.push_back('\0');
  canonical.append(locator);
  canonical.push_back('\0');
  canonical.append(semantic_source);
  return sha256_hex(canonical);
}

auto section(std::string_view body, std::string_view heading) -> std::string_view {
  const auto start = body.find(heading);
  if (start == std::string_view::npos) {
    return {};
  }
  const auto tail = body.substr(start + heading.size());
  const auto end  = tail.find("\n## ");
  return trim(tail.substr(0, end == std::string_view::npos ? tail.size() : end), whitespace_block);
}

auto field(std::string_view body, std::string_view marker) -> std::optional<std::string_view> {
  const auto start = body.find(marker);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const auto tail  = body.substr(start + marker.size());
  const auto end   = tail.find('\n');
  const auto value = trim(tail.substr(0, end == std::string_view::npos ? tail.size() : end), " \t\r");
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

auto artifact_section(std::string_view body, std::string_view locator) -> std::optional<std::string_view> {
  const auto hash = locator.find('#');
  if (hash == std::string_view::npos) {
    return std::nullopt;
  }
  const auto section_name = trim(locator.substr(hash + 1), whitespace_inline);
  if (section_name.empty()) {
    return std::nullopt;
  }

  std::optional<std::size_t> selected_level;
  std::size_t                section_start = 0;
  std::optional<char>        fence_char;
  std::size_t                fence_len = 0;
  std::size_t                offset    = 0;

  while (offset <= body.size()) {
    const auto line_end    = body.find('\n', offset);
    const auto real_end    = line_end == std::string_view::npos ? body.size() : line_end;
    const auto line        = body.substr(offset, real_end - offset);
    const auto next_offset = line_end == std::string_view::npos ? body.size() + 1 : line_end + 1;

    if (const auto delim = fence_delimiter(line); delim.has_value()) {
      if (!fence_char.has_value()) {
        fence_char = delim->char_;
        fence_len  = delim->len_;
      } else if (delim->char_ == *fence_char && delim->len_ >= fence_len && delim->is_closing_) {
        fence_char.reset();
        fence_len = 0;
      }
      offset = next_offset;
      continue;
    }

    if (!fence_char.has_value() && !is_indented_code(line)) {
      if (const auto head = atx_heading(line); head.has_value()) {
        if (selected_level.has_value()) {
          // A heading at the same or a shallower level closes the section; a
          // deeper one is nested content and stays inside it.
          if (head->level_ <= *selected_level) {
            return trim(body.substr(section_start, offset - section_start), whitespace_block);
          }
        } else if (equals_fold(head->title_, section_name)) {
          selected_level = head->level_;
          section_start  = std::min(next_offset, body.size());
        }
      }
    }

    if (real_end == body.size()) {
      break;
    }
    offset = next_offset;
  }

  if (!selected_level.has_value()) {
    return std::nullopt;
  }
  return trim(body.substr(section_start), whitespace_block);
}

auto unwrap_stored_artifact_body(std::string_view body) -> std::string_view {
  constexpr std::string_view marker  = "## Content";
  const auto                 trimmed = [&] {
    const auto first = body.find_first_not_of(" \t\r\n");
    return first == std::string_view::npos ? std::string_view{} : body.substr(first);
  }();
  if (!trimmed.starts_with(marker)) {
    return body;
  }

  auto       rest          = trimmed.substr(marker.size());
  const auto after_heading = rest.find('\n');
  if (after_heading == std::string_view::npos) {
    return body;
  }
  rest = rest.substr(after_heading + 1);

  const auto lead_start = rest.find_first_not_of(" \t\r\n");
  const auto lead       = lead_start == std::string_view::npos ? std::string_view{} : rest.substr(lead_start);
  if (!lead.starts_with("---")) {
    // `## Content` with no frontmatter is not the wrapper shape; leave it.
    return body;
  }

  auto       scan       = lead.substr(3);
  const auto after_open = scan.find('\n');
  if (after_open == std::string_view::npos) {
    return body;
  }
  scan = scan.substr(after_open + 1);

  std::size_t offset = 0;
  while (offset < scan.size()) {
    const auto line_end = scan.find('\n', offset);
    const auto real_end = line_end == std::string_view::npos ? scan.size() : line_end;
    if (trim(scan.substr(offset, real_end - offset), " \t\r") == "---") {
      return real_end < scan.size() ? scan.substr(real_end + 1) : std::string_view{};
    }
    offset = real_end < scan.size() ? real_end + 1 : scan.size();
  }
  // Unterminated frontmatter: not the wrapper shape.
  return body;
}

auto roadmap_bullet_for_slug(std::string_view body, std::string_view slug) -> std::optional<std::string_view> {
  constexpr std::string_view marker = "[slug:";
  std::size_t                offset = 0;
  while (offset <= body.size()) {
    const auto line_end = body.find('\n', offset);
    const auto real_end = line_end == std::string_view::npos ? body.size() : line_end;
    const auto trimmed  = trim(body.substr(offset, real_end - offset), " \t\r");
    if (trimmed.starts_with("- ") || trimmed.starts_with("* ")) {
      if (const auto marker_start = trimmed.find(marker); marker_start != std::string_view::npos) {
        const auto after_marker = trimmed.substr(marker_start + marker.size());
        if (const auto close = after_marker.find(']'); close != std::string_view::npos) {
          if (trim(after_marker.substr(0, close), whitespace_inline) == slug) {
            return trimmed;
          }
        }
      }
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    offset = line_end + 1;
  }
  return std::nullopt;
}

auto roadmap_section(std::string_view body, std::string_view locator) -> std::optional<std::string> {
  const auto hash = locator.find('#');
  if (hash == std::string_view::npos) {
    return std::nullopt;
  }
  const auto spec = locator.substr(hash + 1);
  if (!spec.starts_with("milestone:")) {
    return std::nullopt;
  }
  const auto rest  = spec.substr(std::string_view{"milestone:"}.size());
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos) {
    return std::nullopt;
  }
  const auto parse_index = [](std::string_view text) -> std::optional<std::size_t> {
    std::size_t value  = 0;
    const auto  result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
      return std::nullopt;
    }
    return value;
  };
  const auto milestone_no = parse_index(rest.substr(0, slash));
  if (!milestone_no.has_value()) {
    return std::nullopt;
  }
  const auto item_part = rest.substr(slash + 1);
  if (!item_part.starts_with("item:")) {
    return std::nullopt;
  }
  const auto item_no = parse_index(item_part.substr(std::string_view{"item:"}.size()));
  if (!item_no.has_value()) {
    return std::nullopt;
  }
  if (*milestone_no == 0 || *item_no == 0) {
    return std::nullopt;
  }

  const auto milestones = parse::parse_roadmap(unwrap_stored_artifact_body(body));
  if (*milestone_no > milestones.size()) {
    return std::nullopt;
  }
  const auto& ms = milestones[*milestone_no - 1];
  if (*item_no > ms.work_items_.size()) {
    return std::nullopt;
  }
  const auto& item = ms.work_items_[*item_no - 1];
  return item.source_text_.empty() ? item.title_ : item.source_text_;
}

auto reconcile(db::connection& conn, std::int64_t anchor_plan_id, std::span<const roadmap_citation> roadmap_citations)
    -> std::expected<void, materialize_error> {
  if (auto r = create_stage(conn); !r.has_value()) {
    return r;
  }
  if (!conn.execute("delete from temp.routing_task_facts_stage").has_value()) {
    return query_failure();
  }

  if (auto r = stage_task_facts(conn, anchor_plan_id); !r.has_value()) {
    return r;
  }
  if (auto r = stage_roadmap_facts(conn, roadmap_citations); !r.has_value()) {
    return r;
  }
  if (auto r = stage_artifact_facts(conn, anchor_plan_id, roadmap_citations); !r.has_value()) {
    return r;
  }
  if (auto r = stage_decision_facts(conn, anchor_plan_id); !r.has_value()) {
    return r;
  }
  if (auto r = stage_question_facts(conn, anchor_plan_id); !r.has_value()) {
    return r;
  }
  if (auto r = stage_scenario_facts(conn, anchor_plan_id); !r.has_value()) {
    return r;
  }
  if (auto r = stage_link_facts(conn, anchor_plan_id); !r.has_value()) {
    return r;
  }

  const auto unchanged = fact_sets_equal(conn, anchor_plan_id);
  if (!unchanged.has_value()) {
    return std::unexpected(unchanged.error());
  }
  if (*unchanged) {
    return {};
  }

  {
    auto stmt = conn.prepare(R"(delete from routing_task_facts
where task_id in (
  select t.id from tasks t
  join plans p on p.id = t.plan_id
  where p.parent_plan_id = ?
))");
    if (!stmt.has_value() || !stmt->bind_int64(1, anchor_plan_id).has_value() || !stmt->step().has_value()) {
      return query_failure();
    }
  }

  // `distinct` plus the explicit ORDER BY are contractual: they make the
  // rewritten row order deterministic, so a replay that DOES change one fact
  // does not reshuffle every unrelated row's id along with it.
  if (!conn.execute(R"(insert into routing_task_facts (
  task_id, fact_kind, value_type, value_bool, value_integer,
  value_real, value_text, source_entity_kind, source_entity_id,
  source_locator, source_digest, materializer_version
)
select distinct task_id, fact_kind, value_type, value_bool, value_integer,
       null, value_text, source_entity_kind, source_entity_id,
       source_locator, source_digest, materializer_version
from temp.routing_task_facts_stage
order by task_id, source_entity_kind, source_entity_id, source_locator,
         fact_kind, value_type, coalesce(value_text, ''),
         coalesce(value_integer, value_bool))")
           .has_value()) {
    return query_failure();
  }
  return {};
}

} // namespace planar::engine::ingest::materialize

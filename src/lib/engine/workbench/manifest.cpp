/// @file manifest.cpp
/// @brief Implementation of `planar.engine.workbench.manifest` (plan 996,
/// task 6037), including the bucket-local SHA-256. See manifest.cppm for why
/// the digest is implemented here rather than vendored.

module planar.engine.workbench.manifest;

import std;
import planar.db;
import planar.engine.workbench.fsutil;

namespace planar::engine::workbench::manifest {

namespace {

// ---------------------------------------------------------------------------
// SHA-256 (FIPS 180-4). Verified against the oracle's own `.sync` digests
// and against `shasum -a 256` in manifest.t.cpp.

constexpr std::array<std::uint32_t, 64> k_round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

auto rotr(std::uint32_t value, unsigned bits) -> std::uint32_t {
  return (value >> bits) | (value << (32U - bits));
}

auto sha256_digest(std::string_view content) -> std::array<std::uint8_t, 32> {
  std::array<std::uint32_t, 8> state{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                     0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  std::vector<std::uint8_t> message(content.size());
  std::ranges::transform(content, message.begin(), [](char c) { return static_cast<std::uint8_t>(c); });
  auto const bit_length = static_cast<std::uint64_t>(message.size()) * 8U;
  message.push_back(0x80U);
  while (message.size() % 64U != 56U) {
    message.push_back(0x00U);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
  }

  std::array<std::uint32_t, 64> schedule{};
  for (std::size_t offset = 0; offset < message.size(); offset += 64U) {
    for (std::size_t i = 0; i < 16U; ++i) {
      schedule.at(i) = (static_cast<std::uint32_t>(message[offset + (i * 4U)]) << 24U) |
                       (static_cast<std::uint32_t>(message[offset + (i * 4U) + 1U]) << 16U) |
                       (static_cast<std::uint32_t>(message[offset + (i * 4U) + 2U]) << 8U) |
                       static_cast<std::uint32_t>(message[offset + (i * 4U) + 3U]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
      auto const s0  = rotr(schedule.at(i - 15U), 7U) ^ rotr(schedule.at(i - 15U), 18U) ^ (schedule.at(i - 15U) >> 3U);
      auto const s1  = rotr(schedule.at(i - 2U), 17U) ^ rotr(schedule.at(i - 2U), 19U) ^ (schedule.at(i - 2U) >> 10U);
      schedule.at(i) = schedule.at(i - 16U) + s0 + schedule.at(i - 7U) + s1;
    }

    auto working = state;
    for (std::size_t i = 0; i < 64U; ++i) {
      auto const s1 = rotr(working[4], 6U) ^ rotr(working[4], 11U) ^ rotr(working[4], 25U);
      auto const ch = (working[4] & working[5]) ^ (~working[4] & working[6]);
      auto const t1 = working[7] + s1 + ch + k_round_constants.at(i) + schedule.at(i);
      auto const s0 = rotr(working[0], 2U) ^ rotr(working[0], 13U) ^ rotr(working[0], 22U);
      auto const mj = (working[0] & working[1]) ^ (working[0] & working[2]) ^ (working[1] & working[2]);
      auto const t2 = s0 + mj;
      working[7]    = working[6];
      working[6]    = working[5];
      working[5]    = working[4];
      working[4]    = working[3] + t1;
      working[3]    = working[2];
      working[2]    = working[1];
      working[1]    = working[0];
      working[0]    = t1 + t2;
    }
    for (std::size_t i = 0; i < 8U; ++i) {
      state.at(i) += working.at(i);
    }
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t i = 0; i < 8U; ++i) {
    digest.at(i * 4U)        = static_cast<std::uint8_t>((state.at(i) >> 24U) & 0xffU);
    digest.at((i * 4U) + 1U) = static_cast<std::uint8_t>((state.at(i) >> 16U) & 0xffU);
    digest.at((i * 4U) + 2U) = static_cast<std::uint8_t>((state.at(i) >> 8U) & 0xffU);
    digest.at((i * 4U) + 3U) = static_cast<std::uint8_t>(state.at(i) & 0xffU);
  }
  return digest;
}

constexpr std::string_view k_select_columns = "select id, anchor_plan_id, entity_kind, entity_id, file_path, content_hash, "
                                              "coalesce(fs_mtime, ''), coalesce(db_updated_at, ''), coalesce(last_synced_at, '') "
                                              "from workbench_sync_state where anchor_plan_id = ? order by file_path";

} // namespace

auto hash_content(std::string_view content) -> std::string {
  auto const  digest = sha256_digest(content);
  std::string out;
  out.reserve(64);
  for (auto const byte : digest) {
    std::format_to(std::back_inserter(out), "{:02x}", byte);
  }
  return out;
}

auto load(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<std::vector<sync_state>, manifest_error> {
  auto stmt = conn.prepare(k_select_columns);
  if (!stmt) {
    return std::unexpected(manifest_error::query_failed);
  }
  if (!stmt->bind_int64(1, anchor_plan_id)) {
    return std::unexpected(manifest_error::query_failed);
  }
  std::vector<sync_state> out;
  while (true) {
    auto stepped = stmt->step();
    if (!stepped) {
      return std::unexpected(manifest_error::query_failed);
    }
    if (*stepped == db::step_result::done) {
      break;
    }
    out.push_back(sync_state{
        .id             = stmt->column_int64(0),
        .anchor_plan_id = stmt->column_int64(1),
        .entity_kind    = stmt->column_text(2),
        .entity_id      = stmt->column_int64(3),
        .file_path      = stmt->column_text(4),
        .content_hash   = stmt->column_text(5),
        .fs_mtime       = stmt->column_text(6),
        .db_updated_at  = stmt->column_text(7),
        .last_synced_at = stmt->column_text(8),
    });
  }
  return out;
}

auto upsert(db::connection& conn, const sync_state& row) -> std::expected<void, manifest_error> {
  auto stmt = conn.prepare("insert into workbench_sync_state "
                           "(anchor_plan_id, entity_kind, entity_id, file_path, content_hash, fs_mtime, "
                           "db_updated_at, last_synced_at) "
                           "values (?, ?, ?, ?, ?, ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now')) "
                           "on conflict(file_path) do update set "
                           "  anchor_plan_id = excluded.anchor_plan_id, "
                           "  entity_kind = excluded.entity_kind, "
                           "  entity_id = excluded.entity_id, "
                           "  content_hash = excluded.content_hash, "
                           "  fs_mtime = excluded.fs_mtime, "
                           "  db_updated_at = excluded.db_updated_at, "
                           "  last_synced_at = excluded.last_synced_at");
  if (!stmt) {
    return std::unexpected(manifest_error::query_failed);
  }
  bool const bound = stmt->bind_int64(1, row.anchor_plan_id).has_value() && stmt->bind_text(2, row.entity_kind).has_value() &&
                     stmt->bind_int64(3, row.entity_id).has_value() && stmt->bind_text(4, row.file_path).has_value() &&
                     stmt->bind_text(5, row.content_hash).has_value() && stmt->bind_text(6, row.fs_mtime).has_value() &&
                     stmt->bind_text(7, row.db_updated_at).has_value();
  if (!bound) {
    return std::unexpected(manifest_error::query_failed);
  }
  if (!stmt->step()) {
    return std::unexpected(manifest_error::query_failed);
  }
  return {};
}

auto delete_by_file_path(db::connection& conn, std::string_view file_path) -> std::expected<void, manifest_error> {
  auto stmt = conn.prepare("delete from workbench_sync_state where file_path = ?");
  if (!stmt || !stmt->bind_text(1, file_path) || !stmt->step()) {
    return std::unexpected(manifest_error::query_failed);
  }
  return {};
}

auto delete_by_entity(db::connection& conn, std::int64_t anchor_plan_id, std::string_view entity_kind, std::int64_t entity_id)
    -> std::expected<void, manifest_error> {
  auto stmt = conn.prepare("delete from workbench_sync_state where anchor_plan_id = ? and entity_kind = ? and entity_id = ?");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->bind_text(2, entity_kind) || !stmt->bind_int64(3, entity_id) ||
      !stmt->step()) {
    return std::unexpected(manifest_error::query_failed);
  }
  return {};
}

auto delete_for_plan(db::connection& conn, std::int64_t anchor_plan_id) -> std::expected<void, manifest_error> {
  auto stmt = conn.prepare("delete from workbench_sync_state where anchor_plan_id = ?");
  if (!stmt || !stmt->bind_int64(1, anchor_plan_id) || !stmt->step()) {
    return std::unexpected(manifest_error::query_failed);
  }
  return {};
}

auto feature_rel_path(std::string_view file_path) -> std::string_view {
  auto trimmed = file_path;
  while (!trimmed.empty() && trimmed.front() == '/') {
    trimmed.remove_prefix(1);
  }
  while (!trimmed.empty() && trimmed.back() == '/') {
    trimmed.remove_suffix(1);
  }
  auto const first = trimmed.find('/');
  if (first == std::string_view::npos) {
    return trimmed;
  }
  auto const second = trimmed.find('/', first + 1);
  if (second == std::string_view::npos) {
    return trimmed;
  }
  return trimmed.substr(second + 1);
}

auto render_sync_file(std::span<const sync_state> rows) -> std::string {
  std::string out;
  for (auto const& row : rows) {
    std::format_to(std::back_inserter(out), "{}\t{}:{}\t{}\t{}\n", feature_rel_path(row.file_path), row.entity_kind,
                   row.entity_id, row.content_hash, row.last_synced_at);
  }
  return out;
}

auto write_sync_file(const std::filesystem::path& feature_dir, std::span<const sync_state> rows) -> bool {
  return fsutil::write_file_atomic(feature_dir / ".sync", render_sync_file(rows));
}

} // namespace planar::engine::workbench::manifest

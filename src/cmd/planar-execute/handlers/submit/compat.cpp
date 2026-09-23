/// @file compat.cpp
/// @brief Implementation of the compatibility tuple (plan 1033 M2, task 6503).
module;

#include <glaze/glaze.hpp>

module planar.cmd.planar_execute.compat;

import std;
import planar.cmd.planar_execute.host;
import planar.cmd.planar_execute.profile;
import planar.sha256;

namespace planar::cmd::execute {

/// @brief Glaze-reflected shapes. Named namespace: Glaze cannot reflect a type
/// with no linkage.
namespace wire {

/// @brief The recorded tuple, as it sits beside the profile's state.
struct compatibility_wire {
  std::string protocol_version;      ///< Wire contract the recording client expected.
  std::string daemon_build;          ///< Installed daemon's build identity.
  std::string bundle_digest;         ///< Digest over the profile's bundle directory.
  std::string command_policy_digest; ///< Digest of the command-policy file.
  std::string planar_db;             ///< Canonical path of the Planar database.
  std::string workbench_root;        ///< Canonical workbench root.
  std::string sibling_bin_dir;       ///< Directory the Planar binaries were invoked from.
};

/// @brief The fields this client reads from the daemon's build identity.
struct build_identity_wire {
  std::string tag;           ///< Pinned Centurion tag the daemon was built from.
  std::string binary_sha256; ///< Digest of the installed daemon binary.
};

} // namespace wire

namespace {

/// @brief The file the tuple is recorded in, beside the profile's state.
constexpr std::string_view record_name = "planar-compat.json";

/// @brief Read a whole file, or the empty string when unreadable.
auto read_all(const std::filesystem::path& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief Canonical path text, falling back to the input when it cannot be resolved.
///
/// An unresolvable path is still compared, verbatim: two clients disagreeing
/// about a path that does not exist yet is exactly as load-bearing as
/// disagreeing about one that does.
auto canonical_text(const std::filesystem::path& path) -> std::string {
  if (path.empty()) {
    return {};
  }
  std::error_code failure;
  const auto      resolved = std::filesystem::weakly_canonical(path, failure);
  return failure ? path.string() : resolved.string();
}

} // namespace

auto directory_digest(const std::filesystem::path& directory) -> std::string {
  std::error_code failure;
  if (!std::filesystem::is_directory(directory, failure)) {
    return {};
  }
  // Sorted, so the digest is a property of the CONTENT and not of the order a
  // filesystem happened to hand the entries back.
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, failure)) {
    if (entry.is_regular_file(failure)) {
      files.push_back(entry.path());
    }
  }
  std::ranges::sort(files);

  std::string accumulated;
  for (const auto& file : files) {
    const auto relative = std::filesystem::relative(file, directory, failure).generic_string();
    // Path AND bytes: a file renamed with identical content must change the
    // digest, and a digest over contents alone would not notice.
    accumulated += std::format("{}\n{}\n", relative, sha256::hex(read_all(file)));
  }
  return sha256::hex(accumulated);
}

auto compute_tuple(const profile& resolved, const compatibility_inputs& inputs) -> compatibility_tuple {
  // The daemon's build identity comes from the file install-centuriond.sh
  // wrote beside the binary (task 6709), so a rebuilt or re-pinned daemon is a
  // different identity even at the same path.
  std::string daemon_build;
  const auto  identity_path = inputs.daemon_.parent_path().parent_path() / "share" / "centurion" / "build-identity.json";
  if (const auto text = read_all(identity_path); !text.empty()) {
    wire::build_identity_wire identity;
    if (!glz::read<glz::opts{.error_on_unknown_keys = false}>(identity, text)) {
      daemon_build = std::format("{}+{}", identity.tag, identity.binary_sha256);
    }
  }
  if (daemon_build.empty()) {
    // No identity file: name the binary itself rather than leaving the field
    // empty, so two different installed daemons still compare unequal.
    daemon_build = canonical_text(inputs.daemon_);
  }

  return compatibility_tuple{
      .protocol_version_ = inputs.protocol_version_,
      .daemon_build_     = daemon_build,
      .bundle_digest_    = resolved.bundle.has_value() ? directory_digest(*resolved.bundle) : std::string{},
      .command_policy_digest_ =
          resolved.command_policy.has_value() ? sha256::hex(read_all(*resolved.command_policy)) : std::string{},
      .planar_db_       = canonical_text(resolved.planar_db),
      .workbench_root_  = canonical_text(inputs.workbench_root_),
      .sibling_bin_dir_ = canonical_text(inputs.sibling_bin_dir_),
  };
}

auto record_tuple(const host_layout& layout, const compatibility_tuple& tuple) -> std::expected<void, std::string> {
  const wire::compatibility_wire record{.protocol_version      = tuple.protocol_version_,
                                        .daemon_build          = tuple.daemon_build_,
                                        .bundle_digest         = tuple.bundle_digest_,
                                        .command_policy_digest = tuple.command_policy_digest_,
                                        .planar_db             = tuple.planar_db_,
                                        .workbench_root        = tuple.workbench_root_,
                                        .sibling_bin_dir       = tuple.sibling_bin_dir_};
  std::string                    text;
  if (glz::write_json(record, text)) {
    return std::unexpected("cannot render the compatibility record");
  }
  std::error_code failure;
  std::filesystem::create_directories(layout.home_, failure);
  const auto    path = layout.home_ / record_name;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return std::unexpected(std::format("cannot write {}", path.string()));
  }
  out << text;
  out.close();
  if (!out) {
    return std::unexpected(std::format("cannot write {}", path.string()));
  }
  return {};
}

auto recorded_tuple(const host_layout& layout) -> std::optional<compatibility_tuple> {
  const auto text = read_all(layout.home_ / record_name);
  if (text.empty()) {
    return std::nullopt;
  }
  wire::compatibility_wire record;
  if (glz::read<glz::opts{.error_on_unknown_keys = false}>(record, text)) {
    return std::nullopt;
  }
  return compatibility_tuple{.protocol_version_      = record.protocol_version,
                             .daemon_build_          = record.daemon_build,
                             .bundle_digest_         = record.bundle_digest,
                             .command_policy_digest_ = record.command_policy_digest,
                             .planar_db_             = record.planar_db,
                             .workbench_root_        = record.workbench_root,
                             .sibling_bin_dir_       = record.sibling_bin_dir};
}

auto compare_tuples(const std::optional<compatibility_tuple>& recorded, const compatibility_tuple& current)
    -> std::vector<compatibility_mismatch> {
  if (!recorded.has_value()) {
    return {};
  }
  std::vector<compatibility_mismatch> mismatches;
  const auto note = [&mismatches](std::string_view field, const std::string& was, const std::string& is) {
    if (was != is) {
      mismatches.push_back(compatibility_mismatch{.field_ = std::string(field), .recorded_ = was, .current_ = is});
    }
  };
  note("protocol_version", recorded->protocol_version_, current.protocol_version_);
  note("daemon_build", recorded->daemon_build_, current.daemon_build_);
  note("bundle_digest", recorded->bundle_digest_, current.bundle_digest_);
  note("command_policy_digest", recorded->command_policy_digest_, current.command_policy_digest_);
  note("planar_db", recorded->planar_db_, current.planar_db_);
  note("workbench_root", recorded->workbench_root_, current.workbench_root_);
  note("sibling_bin_dir", recorded->sibling_bin_dir_, current.sibling_bin_dir_);
  return mismatches;
}

auto mismatch_text(std::span<const compatibility_mismatch> mismatches, std::string_view profile_name) -> std::string {
  std::string text = std::format("the daemon serving profile '{}' was started with a different identity:", profile_name);
  for (const auto& mismatch : mismatches) {
    // Both values, always: "they differ" without saying how is a diagnostic an
    // operator cannot act on.
    text += std::format("\n  {}: running {}, this client {}", mismatch.field_,
                        mismatch.recorded_.empty() ? "<unset>" : mismatch.recorded_,
                        mismatch.current_.empty() ? "<unset>" : mismatch.current_);
  }
  return text;
}

} // namespace planar::cmd::execute

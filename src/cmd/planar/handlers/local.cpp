/// @file local.cpp
/// @brief Implementation of `planar.cmd.planar.handlers.local`. See local.cppm
/// for the environment seam and the four places the orchestration is
/// load-bearing.

module planar.cmd.planar.handlers.local;

import std;
import cli11;
import planar.cliapp.args;
import planar.engine.local;
import planar.cmd.planar.context;
import planar.cmd.planar.exit;
import planar.cmd.planar.handler;

namespace planar::cmd::handlers {

namespace manifest = engine::local::manifest;
namespace link_ns  = engine::local::link;
namespace import_  = engine::local::import_;
namespace render   = engine::local::render;

namespace {

/// @brief Resolve `$PLANAR_LOCAL_HOME` / `$HOME` into the sandbox roots.
///
/// The refusal transcribes the Zig `@errorName` (`HomeNotSet`) rather than
/// capturing it, for the reason `workspace_doctor` gives about its own
/// transcribed tag: the path needs BOTH variables unset to reach, which no
/// oracle probe under the pinned parity arena can produce (the harness always
/// sets both). Everything reachable IS captured.
/// @param ctx The invocation context.
/// @return The roots, or the exit-1 refusal.
auto sandbox_roots(context& ctx) -> std::expected<manifest::home_and_root, domain_error> {
  auto resolved = manifest::resolve_home_and_root(ctx.env());
  if (!resolved.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "resolving sandbox root: HomeNotSet"));
  }
  return *resolved;
}

/// @brief Emit one source's `local link` block in whichever mode was asked for.
/// @param ctx The invocation context.
/// @param file The parsed source.
/// @param result What link() did.
/// @param as_json Whether `--json` was passed.
auto emit_link_block(context& ctx, const manifest::sandbox_file& file, const link_ns::link_result& result, bool as_json) -> void {
  ctx.out() << (as_json ? render::link_json(file, result) : render::link_source_text(result));
}

/// @brief Serve `local link --reconcile`.
///
/// Split out because the oracle branches on `--reconcile` BEFORE resolving
/// anything else except the roots, and because its refusal for a stray
/// positional is checked first of all — `local link --reconcile foo` is exit 2
/// even when `$HOME` is unset.
/// @param ctx The invocation context.
/// @param dry_run Whether `--dry-run` was passed.
/// @param as_json Whether `--json` was passed.
/// @return Success, or the exit-1 refusal when a manifest could not be read.
auto run_reconcile(context& ctx, bool dry_run, bool as_json) -> handler_result {
  auto roots = sandbox_roots(ctx);
  if (!roots) {
    return std::unexpected(roots.error());
  }
  auto actions = link_ns::reconcile({.home_dir = roots->home_dir, .dry_run = dry_run, .now = link_ns::utc_now_stamp()});
  if (!actions.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "reconcile failed: ManifestUnreadable"));
  }
  ctx.out() << (as_json ? render::reconcile_json(*actions) : render::reconcile_text(*actions, dry_run));
  return {};
}

} // namespace

auto local_list(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto roots = sandbox_roots(ctx);
  if (!roots) {
    return std::unexpected(roots.error());
  }
  auto rows = link_ns::list(roots->home_dir);
  if (!rows.has_value()) {
    return std::unexpected(error_from_body(domain_error_kind::generic_failure, "listing sandbox installs: ManifestUnreadable"));
  }

  auto const                      raw = cliapp::flag_string(args, "--vendor");
  std::optional<std::string_view> vendor;
  if (raw.has_value()) {
    vendor = std::string_view{*raw};
  }

  // Both renderers return COMPLETE payloads, and they disagree on the empty
  // case in TWO directions at once: `--json` is ZERO BYTES where text is a
  // sentence, and the two empty cases in text mode ("nothing recorded" versus
  // "nothing matched the filter") are different sentences. Appending anything
  // here would break the first of those.
  ctx.out() << (cliapp::flag_bool(args, "--json") ? render::list_json(*rows, vendor) : render::list_text(*rows, vendor));
  return {};
}

auto local_link(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto const dry_run = cliapp::flag_bool(args, "--dry-run");
  auto const as_json = cliapp::flag_bool(args, "--json");
  auto const name    = cliapp::positional_string(args, "name");

  if (cliapp::flag_bool(args, "--reconcile")) {
    if (name.has_value()) {
      // The renderer returns the WHOLE line, `error: ` prefix included.
      return std::unexpected(
          error_from_rendered(domain_error_kind::invalid_input, render::reconcile_takes_no_positional_error()));
    }
    return run_reconcile(ctx, dry_run, as_json);
  }

  auto roots = sandbox_roots(ctx);
  if (!roots) {
    return std::unexpected(roots.error());
  }

  auto const walked = manifest::walk_sandbox(roots->sandbox_root);

  // TEXT MODE ONLY. Under `--json` a malformed sandbox source is silently
  // dropped — the oracle's asymmetry, preserved. See local.cppm.
  if (!as_json) {
    // NOTE the SET of these lines is the contract, not their order: the walk
    // appends in directory-iteration order on both sides, and that order is
    // unspecified. Tests fixture exactly one walk error for this reason.
    for (auto const& problem : walked.walk_errors) {
      ctx.out() << render::walk_error_text(problem);
    }
  }

  std::vector<manifest::sandbox_file> picked;
  for (auto const& file : walked.files) {
    if (name.has_value() && file.name != *name) {
      continue;
    }
    picked.push_back(file);
  }

  if (name.has_value() && picked.empty()) {
    return std::unexpected(error_from_rendered(domain_error_kind::generic_failure,
                                               render::no_such_source_error(*name, roots->sandbox_root.string())));
  }
  if (picked.empty()) {
    // Exit 0, not an error: an operator with no personal skills yet is not in
    // a failure state.
    ctx.out() << render::no_sources_text(roots->sandbox_root.string());
    return {};
  }

  // ONE clock read for the whole run, so a multi-source link cannot stamp two
  // sources a second apart. link() takes it as a parameter precisely so this
  // is the only place it is read.
  auto const stamp  = link_ns::utc_now_stamp();
  auto const vendor = cliapp::flag_string(args, "--vendor");

  std::vector<link_ns::link_result> results;
  results.reserve(picked.size());
  for (auto const& file : picked) {
    if (!as_json) {
      for (auto const& issue : manifest::lint(file.frontmatter, file.name)) {
        ctx.out() << render::lint_text(issue, file.kind, file.name);
      }
    }
    auto result = link_ns::link(
        file, {.home_dir = roots->home_dir, .dry_run = dry_run, .vendor_filter = vendor, .force_copy = false, .now = stamp});
    emit_link_block(ctx, file, result, as_json);
    results.push_back(std::move(result));
  }

  if (!as_json) {
    ctx.out() << render::link_summary_text(results, dry_run);
  }
  return {};
}

auto local_unlink(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto roots = sandbox_roots(ctx);
  if (!roots) {
    return std::unexpected(roots.error());
  }
  auto const name    = cliapp::positional_string(args, "name").value_or(std::string{});
  auto const purge   = cliapp::flag_bool(args, "--purge");
  auto const as_json = cliapp::flag_bool(args, "--json");

  // The kind FALLBACK. A name whose source is already gone resolves to
  // neither kind, and the oracle then unlinks BOTH manifests rather than
  // refusing — which is what makes `unlink` recover an orphaned install.
  std::vector<manifest::kind> kinds;
  if (auto const guessed = manifest::lookup_kind_for_name(roots->sandbox_root, name); guessed.has_value()) {
    kinds.push_back(*guessed);
  } else {
    kinds.push_back(manifest::kind::skill);
    kinds.push_back(manifest::kind::agent);
  }

  std::size_t total = 0;
  for (auto const unlink_kind : kinds) {
    auto result = link_ns::unlink(name, unlink_kind, {.home_dir = roots->home_dir, .purge = purge});
    if (!result.has_value()) {
      return std::unexpected(
          error_from_body(domain_error_kind::generic_failure, std::format("unlinking {} failed: ManifestUnreadable", name)));
    }
    // A pass that removed nothing AND purged nothing prints nothing at all —
    // not an empty block. That is what lets the both-kinds fallback stay
    // quiet about the kind that did not match.
    if (result->removed.empty() && result->purged_file.empty()) {
      continue;
    }
    ctx.out() << (as_json ? render::unlink_json(*result) : render::unlink_text(*result));
    total += result->removed.size();
  }

  if (!as_json && total == 0) {
    ctx.out() << render::unlink_none_text(name);
  }
  return {};
}

auto local_import(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto roots = sandbox_roots(ctx);
  if (!roots) {
    return std::unexpected(roots.error());
  }

  auto const                      raw_kind = cliapp::flag_string(args, "--kind");
  std::optional<std::string_view> kind_view;
  if (raw_kind.has_value()) {
    kind_view = std::string_view{*raw_kind};
  }
  auto const kind = manifest::parse_kind(kind_view);
  if (!kind.has_value()) {
    return std::unexpected(
        error_from_rendered(domain_error_kind::invalid_input, render::invalid_kind_error(raw_kind.value_or(std::string{}))));
  }

  auto const path    = cliapp::positional_string(args, "path").value_or(std::string{});
  auto const dry_run = cliapp::flag_bool(args, "--dry-run");
  auto const as_json = cliapp::flag_bool(args, "--json");

  auto const outcome = import_::import_sources({.home_dir    = roots->home_dir,
                                                .source_path = path,
                                                .kind        = *kind,
                                                .force       = cliapp::flag_bool(args, "--force"),
                                                .dry_run     = dry_run});
  if (!outcome) {
    // The two failures land in DIFFERENT exit buckets, exactly as the Zig
    // `exit.die` mapping does: `InvalidInput` is exit 2, `NotFound` exit 1.
    // Collapsing them would make "you pointed at a .txt" and "that directory
    // held no importable files" indistinguishable to a script.
    auto const [bucket, tag] = outcome.error() == import_::import_error::invalid_input
                                   ? std::pair{domain_error_kind::invalid_input, "InvalidInput"}
                                   : std::pair{domain_error_kind::generic_failure, "NotFound"};
    return std::unexpected(error_from_body(bucket, std::format("importing {} failed: {}", path, tag)));
  }

  ctx.out() << (as_json ? render::import_json(*outcome) : render::import_text(*outcome));

  if (cliapp::flag_bool(args, "--no-link") || dry_run || outcome->imported.empty()) {
    return {};
  }

  // Phase two: RE-PARSE each imported file from its DESTINATION and link it.
  // Re-parsing rather than reusing the import's own parse is the oracle's
  // choice and it matters: a skill imported from a flat `<name>.md` lands as
  // `<name>/SKILL.md`, so the source_path recorded in the manifest has to be
  // the destination's, not the origin's.
  if (!as_json) {
    ctx.out() << "\nLinking imported files into vendor surfaces:\n";
  }
  auto const stamp = link_ns::utc_now_stamp();
  for (auto const& record : outcome->imported) {
    auto const parsed = manifest::parse_file(record.target_path, *kind);
    if (!parsed) {
      return std::unexpected(error_from_body(
          domain_error_kind::generic_failure,
          std::format("re-parsing imported file {} failed: {}", record.target_path, manifest::parse_error_name(parsed.error()))));
    }
    auto const result = link_ns::link(*parsed, {.home_dir = roots->home_dir, .now = stamp});
    emit_link_block(ctx, *parsed, result, as_json);
  }
  return {};
}

auto local_migrate(context& ctx, const cliapp::parsed_args& args) -> handler_result {
  auto roots = sandbox_roots(ctx);
  if (!roots) {
    return std::unexpected(roots.error());
  }
  auto const dry_run = cliapp::flag_bool(args, "--dry-run");
  auto const outcome = manifest::migrate(roots->sandbox_root, dry_run);

  // The JSON form carries NO dry-run marker, so a scripted caller cannot tell
  // a rehearsal from a real run. Oracle-confirmed by diffing the two captures
  // byte for byte; preserved rather than corrected.
  ctx.out() << (cliapp::flag_bool(args, "--json") ? render::migrate_json(outcome) : render::migrate_text(outcome, dry_run));
  return {};
}

} // namespace planar::cmd::handlers

/// @file cli_log.cpp
/// @brief Implementation of `planar.cmd.planar.cli_log` (see cli_log.cppm
/// for the derived privacy boundary this file has to hold).
module;

module planar.cmd.planar.cli_log;

import std;
import cli11;
import planar.db;
import planar.engine.config.effective;
import planar.cmd.planar.context;
import planar.cmd.internal.config_path;
import planar.cmd.planar.exit;
import planar.cmd.planar.main;
import planar.cliapp.walk;

namespace planar::cmd {

namespace {

namespace cfg = engine::config;

/// @brief Whether `token` is a long flag (`--name`, including a bare `--`).
/// @param token The argv token.
/// @return `true` for a `--`-prefixed token.
auto is_long_flag(std::string_view token) -> bool {
  return token.starts_with("--");
}

/// @brief Whether `token` is a short flag (`-x`, but not `-` or `--...`).
/// @param token The argv token.
/// @return `true` for a short-flag-shaped token.
auto is_short_flag(std::string_view token) -> bool {
  return token.size() >= 2 && token[0] == '-' && token[1] != '-';
}

/// @brief The placeholder recorded for a verb-slot token the live CLI tree
/// does not name at that depth.
constexpr std::string_view unknown_verb = "<unknown>";

/// @brief The placeholder recorded for a flag the resolved verb does not
/// declare.
constexpr std::string_view unknown_flag = "--<unknown>";

/// @brief The live CLI tree, built once for membership checks.
/// @return The root node; borrowed and valid for the process lifetime.
auto live_root() -> const CLI::App& {
  static const std::unique_ptr<CLI::App> root = root_app();
  return *root;
}

/// @brief The visible direct subcommand of `parent` named `name`.
/// @param parent The node to search.
/// @param name The token as typed.
/// @return The child, or `nullptr` when `parent` has no visible child by that name.
auto named_child(const CLI::App& parent, std::string_view name) -> const CLI::App* {
  for (auto const* child : cliapp::children(parent)) {
    if (child->get_name() == name) {
      return child;
    }
  }
  return nullptr;
}

/// @brief Whether any node on `chain` declares the flag spelled `name`.
///
/// Hidden options count: they are declared, and their names are catalog
/// controlled rather than operator typed.
/// @param chain The resolved nodes, root first.
/// @param name The flag name as typed, `--long` or `-s`, value excluded.
/// @return `true` when an option on the chain answers to `name`.
auto flag_declared(std::span<const CLI::App* const> chain, std::string const& name) -> bool {
  for (auto const* node : chain) {
    for (auto const* opt : node->get_options()) {
      if (!opt->get_positional() && opt->check_name(name)) {
        return true;
      }
    }
  }
  return false;
}

} // namespace

/// @brief Top-level verbs that HAVE subcommands, derived once from the live
/// CLI tree.
///
/// DERIVED, NOT LISTED, and derived HERE rather than taken as a parameter.
/// An earlier shape of this took the set as a defaulted argument, which made
/// the function silently lossy for every direct caller that omitted it — the
/// unit tests call `parse_args` directly, and `task add` collapsed to `task`.
/// A default that degrades the result is a trap; deriving it internally means
/// no caller can get it wrong.
/// @return The set of top-level verbs that have subcommands, built once on
/// first call and returned by reference thereafter.
auto parent_verb_set() -> const std::set<std::string, std::less<>>& {
  static const std::set<std::string, std::less<>> verbs = [] {
    std::set<std::string, std::less<>> out;
    auto const                         root = root_app();
    for (auto const* child : cliapp::children(*root)) {
      if (!cliapp::children(*child).empty()) {
        out.insert(child->get_name());
      }
    }
    return out;
  }();
  return verbs;
}

auto parse_args(std::span<const std::string> argv_tail) -> parsed_args_shape {
  auto const&                   parent_verbs = parent_verb_set();
  std::vector<std::string_view> verb_parts;
  std::vector<std::string_view> flag_names;
  // Verb-slot tokens as they will be recorded: the typed token only when the
  // live tree names it (or it is a structured operand), else `<unknown>`.
  std::vector<std::string> recorded_parts;
  // The nodes the verb slot resolved to, root first. Flags are checked
  // against these; an unresolved verb leaves only the root.
  auto const&                  root = live_root();
  std::vector<const CLI::App*> chain{&root};
  std::size_t                  positional_count = 0;

  // Set by the first flag or by `--`; after it, no token can join the verb
  // path, so a positional that happens to be a bare word is counted rather
  // than recorded.
  bool past_verbs = false;

  for (std::size_t i = 0; i < argv_tail.size(); ++i) {
    std::string_view const token = argv_tail[i];

    if (token == "--") {
      // Everything after the separator is a positional. Values discarded.
      past_verbs = true;
      positional_count += argv_tail.size() - (i + 1);
      break;
    }

    if (is_long_flag(token) || is_short_flag(token)) {
      past_verbs = true;

      // The inline form `--flag=value`. Record the NAME only, `=` excluded
      // (this is what the Zig code does and what the oracle's rows show --
      // the Zig comment saying "including =" is wrong about its own code).
      // The value rides inside this token, so no following token is
      // consumed.
      if (is_long_flag(token)) {
        if (auto const eq = token.find('='); eq != std::string_view::npos) {
          flag_names.push_back(token.substr(0, eq));
          continue;
        }
      }

      // A short flag carrying an attached value, in either spelling:
      //   -pVALUE   (value starts at index 2)
      //   -p=VALUE  (value starts at index 3)
      // Both record `-p` and discard the rest. No planar flag declares a
      // short form today; the closure is STRUCTURAL, not conditional on
      // current usage, because the invariant has to hold for whatever
      // arrives.
      if (is_short_flag(token) && token.size() > 2) {
        flag_names.push_back(token.substr(0, 2));
        continue;
      }

      flag_names.push_back(token);

      // A separate following token is this flag's value unless it looks
      // like a flag itself. Consumed and DISCARDED -- never recorded, and
      // not counted as a positional either.
      if (i + 1 < argv_tail.size() && !std::string_view{argv_tail[i + 1]}.starts_with("-")) {
        ++i;
      }
      continue;
    }

    // The verb slot. Token 2 is RECORDED VERBATIM only when it is
    // STRUCTURED — a bare id (`resume 6073`) or an entity ref
    // (`tree plan:42`) — or when token 1 is a verb that has subcommands
    // (`task add`). Free-text operands are dropped (task 6351).
    //
    // Three top-level verbs carry operator prose in that slot, and the leak
    // was measured with cli_log enabled, not reasoned about:
    //
    //     search SECRET-MEDICAL-TERM
    //     import /Users/private/clients/acme-secret
    //     synthesize /Users/private/clients/acme-secret
    //
    // `introspect` selects this column verbatim into `[invocations]`,
    // `[failure tail]` and the JSONL boundary of `planar report`, so a
    // search term or a client directory name could leave the machine inside
    // a diagnostic bundle.
    //
    // SHAPE, not a verb list. A list of "verbs whose positional is prose"
    // is safe until someone adds a verb and forgets; matching the shape of
    // the VALUE is safe by default, and `resume`/`tree` keep the verbatim
    // capture cli_log.cppm's header documents as the oracle's boundary.
    // `parent_verbs` is derived from the live CLI tree for the same reason.
    auto const structured_operand = [](std::string_view t) {
      if (t.empty()) {
        return false;
      }
      auto const colon  = t.find(':');
      auto const digits = [](std::string_view v) {
        return !v.empty() && std::ranges::all_of(v, [](unsigned char c) { return std::isdigit(c) != 0; });
      };
      if (colon == std::string_view::npos) {
        return digits(t);
      }
      auto const kind = t.substr(0, colon);
      return !kind.empty() &&
             std::ranges::all_of(kind, [](unsigned char c) { return std::isalpha(c) != 0 || c == '-' || c == '_'; }) &&
             digits(t.substr(colon + 1));
    };
    const bool verb_slot_open =
        !past_verbs && (verb_parts.empty() || (verb_parts.size() < max_verb_depth &&
                                               (parent_verbs.contains(verb_parts.front()) || structured_operand(token))));
    if (verb_slot_open) {
      const CLI::App* resolved = nullptr;
      if (verb_parts.empty()) {
        resolved = named_child(root, token);
      } else if (chain.size() == 2) {
        resolved = named_child(*chain.back(), token);
      }
      if (resolved != nullptr) {
        chain.push_back(resolved);
        recorded_parts.emplace_back(token);
      } else if (structured_operand(token)) {
        recorded_parts.emplace_back(token);
      } else {
        recorded_parts.emplace_back(unknown_verb);
      }
      verb_parts.push_back(token);
      continue;
    }

    // A positional: past the verb depth, or after the first flag.
    past_verbs = true;
    ++positional_count;
  }

  parsed_args_shape shape;
  for (auto const& part : recorded_parts) {
    if (!shape.verb_path.empty()) {
      shape.verb_path += ' ';
    }
    shape.verb_path += part;
  }

  if (positional_count > 0) {
    shape.args_shape = std::format("<pos:{}>", positional_count);
  }
  for (auto const& flag : flag_names) {
    if (!shape.args_shape.empty()) {
      shape.args_shape += ' ';
    }
    if (flag_declared(chain, std::string{flag})) {
      shape.args_shape += flag;
    } else {
      shape.args_shape += unknown_flag;
    }
  }
  return shape;
}

auto category_for(domain_error_kind kind) -> std::optional<std::string_view> {
  // Exhaustive rather than defaulted, so a new domain_error_kind is a
  // compiler error here instead of silently landing in `internal`. The
  // arms follow zig/src/cmd/planar/cli_log.zig's `categoryFor`: the named
  // errors first, then everything else by its exit-code bucket (2 ->
  // usage, 3/6 -> conflict, 5 -> scope, 7/64 -> internal, else internal).
  switch (kind) {
  case domain_error_kind::invalid_input:
  case domain_error_kind::invalid_entity_ref:
    return "usage";
  case domain_error_kind::parse_error:
    // Not a named arm in the Zig switch: reaches `usage` through the
    // exit-code fallback, this binary mapping every parse failure to 2.
    return "usage";
  case domain_error_kind::scope_mismatch:
    return "scope";
  case domain_error_kind::not_found:
    // Exit 1 (the generic bucket) but its OWN category -- the Zig source
    // calls this out explicitly against parity triage F-exit-code-not-found.
    return "not_found";
  case domain_error_kind::sync_conflict:
  case domain_error_kind::slug_conflict:
  case domain_error_kind::already_exists:
    return "conflict";
  case domain_error_kind::schema_version_ahead:
  case domain_error_kind::not_implemented:
    return "internal";
  case domain_error_kind::schema_version_behind:
  case domain_error_kind::generic_failure:
    // Exit 1, so the fallback's `else` arm: internal.
    return "internal";
  case domain_error_kind::busy_source:
    // A competing source writer is retryable, rather than an operator
    // failure in this process.  Keep it distinct for Explorer's retry UI.
    return "busy";
  }
  return "internal";
}

auto write_invocation(db::connection& conn, const parsed_args_shape& shape, int exit_code,
                      std::optional<std::string_view> category, std::optional<std::int64_t> duration_ms,
                      std::int64_t retention_days) -> bool {
  // Stateless retention prune, piggybacked on the capture write exactly as
  // the oracle does it: SQLite's own date arithmetic decides what expired,
  // and no last-prune bookkeeping is stored anywhere. The cheap indexed
  // count first, so the common case does no delete at all.
  {
    auto const cutoff  = std::format("date('now', '-{} days')", retention_days);
    bool       expired = false;
    {
      // Scoped so the statement is finalized before the DELETE runs.
      auto check = conn.prepare(std::format("select count(*) from cli_invocations where recorded_at < {}", cutoff));
      if (check) {
        auto const row = check->step();
        expired        = row.has_value() && *row == db::step_result::row && check->column_int64(0) > 0;
      }
    }
    if (expired) {
      static_cast<void>(conn.execute(std::format("delete from cli_invocations where recorded_at < {}", cutoff)));
    }
  }

  auto stmt = conn.prepare("insert into cli_invocations"
                           "  (verb_path, args_shape, exit_code, error_category, scope_slug, duration_ms, recorded_at)"
                           " values (?, ?, ?, ?, null, ?, strftime('%Y-%m-%dT%H:%M:%fZ','now'))");
  if (!stmt) {
    return false;
  }
  if (!stmt->bind_text(1, shape.verb_path) || !stmt->bind_text(2, shape.args_shape) ||
      !stmt->bind_int64(3, static_cast<std::int64_t>(exit_code))) {
    return false;
  }
  auto const bound_category = category.has_value() ? stmt->bind_text(4, *category) : stmt->bind_null(4);
  if (!bound_category) {
    return false;
  }
  auto const bound_duration = duration_ms.has_value() ? stmt->bind_int64(5, *duration_ms) : stmt->bind_null(5);
  if (!bound_duration) {
    return false;
  }
  return stmt->step().has_value();
}

auto resolve_config_path(const env_lookup& env) -> std::optional<std::filesystem::path> {
  return internal::resolve_config_path(env);
}

auto record(context& ctx, int exit_code, std::optional<domain_error_kind> kind, std::optional<std::chrono::milliseconds> duration)
    -> void {
  auto const cfg_path = resolve_config_path(ctx.env());
  if (!cfg_path.has_value()) {
    return;
  }

  std::optional<std::string> file_content;
  {
    std::ifstream file(*cfg_path, std::ios::binary);
    if (file) {
      file_content = std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
    // A missing file is not a failure: it means "everything falls through
    // to the embedded defaults", where cli_log is off.
  }

  // `env_view::empty()` rather than `from_process()`, and that is exact
  // rather than a hermeticity concession: BOTH keys this function reads --
  // `introspection.cli_log` and `introspection.retention_days` -- are
  // resolved by `pick_bool`/`pick_int`, which take no environment at all
  // (effective.cpp:279-280; the Zig `pickBool`/`pickInt` likewise). Only
  // the `pick_str` keys have env overrides. So the resolved values here are
  // identical to what the real environment would produce, and this call
  // cannot be perturbed by the operator's shell.
  auto resolved = cfg::resolve(file_content.has_value() ? std::optional<std::string_view>{*file_content} : std::nullopt,
                               cfg::env_view::empty(), std::nullopt);
  if (!resolved) {
    return; // Unparseable config: fail open, log nothing.
  }
  if (!resolved->cfg.introspection.cli_log) {
    return; // Logging off. The default.
  }

  // NEVER CREATE THE DATABASE. `db::connection::open` creates the file, so
  // without this guard a bare `planar --help` on a fresh machine would
  // leave an empty database behind purely as a side effect of telemetry.
  // Logging an invocation is not a reason to bring into being the thing
  // being logged about.
  std::error_code ec;
  if (!std::filesystem::is_regular_file(ctx.db_path(), ec)) {
    return;
  }

  // NEVER MIGRATE either, which is why this opens its own connection rather
  // than calling ctx.db().ensure_db(). The Zig header records the cost of the
  // other choice: because telemetry runs on EVERY invocation, `--help`
  // included, migrating from here silently advanced an operator's live
  // database to a dev build's schema TWICE, breaking every other installed
  // binary with SchemaVersionAhead until the migration was rolled back by
  // hand -- and invisibly, because this path swallows its errors. A
  // database that needs migrating simply goes unlogged, which is the right
  // trade for telemetry.
  auto conn = db::connection::open(ctx.db_path().string());
  if (!conn) {
    return;
  }

  auto const shape    = parse_args(ctx.argv().empty() ? std::span<const std::string>{} : ctx.argv().subspan(1));
  auto const category = exit_code == 0     ? std::nullopt
                        : kind.has_value() ? category_for(*kind)
                                           // Failed with no domain error to classify: the
                                           // Zig arm is `.internal`.
                                           : std::optional<std::string_view>{"internal"};

  static_cast<void>(write_invocation(*conn, shape, exit_code, category,
                                     duration.has_value() ? std::optional<std::int64_t>{duration->count()} : std::nullopt,
                                     resolved->cfg.introspection.retention_days));
}

} // namespace planar::cmd

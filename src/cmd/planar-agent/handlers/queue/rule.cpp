/// @file rule.cpp
/// @brief Implementation of `planar.cmd.planar_agent.handlers.queue.rule`.
/// See rule.cppm for the contract.

module planar.cmd.planar_agent.handlers.queue.rule;

import std;
import planar.cliapp.args;
import planar.cmd.internal.config_path;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.handler;
import planar.engine.config;
import planar.engine.hostqueue.rule;
import planar.engine.identity.scope;

namespace planar::cmd::agent::handlers {

namespace {

namespace cfg = engine::config;

/// @brief The longest the association lookup waits for a competing lock.
constexpr int k_busy_ms = 1000;

/// @brief The user's config file, flattened, plus the reason it could not be used.
struct config_load {
  cfg::toml_map              file;    ///< The flattened document; empty for no file or an unusable one.
  std::optional<std::string> problem; ///< Why the file was not used; unset when it was absent or loaded.
  std::string                path;    ///< The config path, for the `Source:` line; empty when none resolves.
};

/// @brief Read and parse the config file the context's environment names.
///
/// An unresolvable path or a missing file gives the defaults without a problem. A file that is not
/// a regular file, cannot be read or does not parse gives the defaults and a problem.
/// @param ctx The invocation context.
/// @return The document, its path and the problem, if any.
auto load_config(context& ctx) -> config_load {
  config_load out;
  auto const  path = internal::resolve_config_path(ctx.env());
  if (!path.has_value()) {
    return out;
  }
  out.path = path->string();
  std::error_code ec;
  if (!std::filesystem::exists(*path, ec)) {
    return out;
  }
  if (!std::filesystem::is_regular_file(*path, ec)) {
    out.problem = std::format("config path is not a regular file: {}", out.path);
    return out;
  }
  std::ifstream in{*path, std::ios::binary};
  if (!in) {
    out.problem = std::format("cannot read config file: {}", out.path);
    return out;
  }
  std::string const content{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  auto              parsed = cfg::parse_toml(content);
  if (!parsed.has_value()) {
    out.problem =
        std::format("line {}: col {}: TOML parse error: {}", parsed.error().line, parsed.error().column, parsed.error().message);
    return out;
  }
  out.file = std::move(*parsed);
  return out;
}

/// @brief Whether some association's `policy` table sets `use_queue`. Only then does the caller's
/// association change the answer, so only then is the scope looked up.
auto has_association_use_queue(const cfg::toml_map& file) -> bool {
  return std::ranges::any_of(file, [](auto const& entry) {
    return entry.first.starts_with("associations.") && entry.first.ends_with(".policy.use_queue");
  });
}

/// @brief The association the caller's working directory belongs to, or the reason the lookup
/// could not be made. An unset slug with no reason means the directory has no single association.
struct association_lookup {
  std::optional<std::string> slug;   ///< The association slug, when the directory resolves to one.
  std::optional<std::string> reason; ///< Why the lookup failed; unset when it ran.
};

/// @brief Resolve the working directory to an association through a read-only connection.
/// @param ctx The invocation context.
/// @return The slug or the failure reason; never throws and never writes.
auto lookup_association(context& ctx) -> association_lookup {
  auto const db_path = resolve_db_path(ctx.env());
  if (!db_path.has_value()) {
    return {.slug = std::nullopt, .reason = "no database path resolves"};
  }
  std::ostringstream discarded;
  auto               conn = database_policy::open_read_only(*db_path, discarded, k_busy_ms);
  if (!conn.has_value()) {
    return {.slug = std::nullopt, .reason = std::format("database not usable: {}", db_path->string())};
  }
  auto const derived = engine::identity::derive_from_cwd(*conn, ctx.cwd().string());
  if (!derived.has_value()) {
    return {.slug = std::nullopt, .reason = "working directory could not be resolved"};
  }
  return {.slug = derived->scope, .reason = std::nullopt};
}

} // namespace

auto queue_rule(context& ctx, const cliapp::parsed_args& /*args*/) -> handler_outcome {
  engine::hostqueue::rule_policy policy;

  auto const config = load_config(ctx);
  if (config.problem.has_value()) {
    policy.notes.push_back(std::format("policy unreadable: {}", *config.problem));
  } else {
    std::optional<std::string> slug;
    if (has_association_use_queue(config.file)) {
      auto const found = lookup_association(ctx);
      slug             = found.slug;
      if (found.reason.has_value()) {
        policy.notes.push_back(
            std::format("scope not resolved ({}); only the top-level [policy] table was consulted", *found.reason));
      }
    }
    auto const resolved = cfg::effective_policy(config.file, slug);
    if (!resolved.has_value()) {
      auto const& first = resolved.error().front();
      policy.notes.push_back(std::format("policy unreadable: {}: {}", first.key, first.message));
    } else if (resolved->values.use_queue) {
      policy.mandate     = true;
      auto const section = resolved->sources.find("use_queue");
      policy.source =
          std::format("{}, [{}]", config.path, section == resolved->sources.end() ? "policy" : section->second.section);
    }
  }

  auto const text = engine::hostqueue::render_queue_rule(policy);
  ctx.out().write(text.data(), static_cast<std::streamsize>(text.size()));
  return handler_result{};
}

} // namespace planar::cmd::agent::handlers

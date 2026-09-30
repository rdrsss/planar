/// @file guard.cpp
/// @brief Implementation of `planar.engine.hostqueue.guard`. See guard.cppm
/// for the contract.

module planar.engine.hostqueue.guard;

import std;

namespace planar::engine::hostqueue {

namespace {

constexpr std::array<std::string_view, 7> k_launchers{
    "claude", "codex", "gemini", "copilot", "aider", "opencode", "cursor-agent",
};

/// @brief The last path component of `path`, ignoring trailing slashes.
auto basename_of(std::string_view path) -> std::string_view {
  while (path.size() > 1 && path.ends_with('/')) {
    path.remove_suffix(1);
  }
  auto const slash = path.rfind('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// @brief Whether `a` and `b` are equal when ASCII letters are compared
/// without regard to case. Non-ASCII bytes compare exactly.
auto equals_ignore_case(std::string_view a, std::string_view b) -> bool {
  return std::ranges::equal(a, b, [](char x, char y) {
    auto const fold = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    return fold(x) == fold(y);
  });
}

/// @brief Whether `word` is a `NAME=value` assignment. Before `env` the name
/// is a shell identifier; after `env`, which accepts any name, it is any
/// non-empty text before the first `=`.
auto is_assignment(std::string_view word, bool after_env) -> bool {
  auto const equals = word.find('=');
  if (equals == std::string_view::npos || equals == 0) {
    return false;
  }
  if (after_env) {
    return true;
  }
  auto const name = word.substr(0, equals);
  if (std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
    return false;
  }
  return std::ranges::all_of(name, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; });
}

/// @brief Splits an `env -S` string into words: white space separates, single
/// and double quotes group, and a backslash takes the next character
/// literally.
auto split_words(std::string_view text) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string              current;
  bool                     open  = false;
  char                     quote = '\0';
  for (std::size_t i = 0; i < text.size(); ++i) {
    char const c = text[i];
    if (quote != '\0') {
      if (c == quote) {
        quote = '\0';
      } else if (c == '\\' && quote == '"' && i + 1 < text.size()) {
        current += text[++i];
      } else {
        current += c;
      }
    } else if (c == '\'' || c == '"') {
      quote = c;
      open  = true;
    } else if (c == '\\' && i + 1 < text.size()) {
      current += text[++i];
      open = true;
    } else if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (open || !current.empty()) {
        out.push_back(std::move(current));
        current.clear();
        open = false;
      }
    } else {
      current += c;
      open = true;
    }
  }
  if (open || !current.empty()) {
    out.push_back(std::move(current));
  }
  return out;
}

/// @brief Replaces the `count` words at `at` with the words of `split`.
void splice_split(std::vector<std::string>& words, std::size_t at, std::size_t count, std::string_view split) {
  auto const inserted = split_words(split);
  auto const first    = words.begin() + static_cast<std::ptrdiff_t>(at);
  words.erase(first, first + static_cast<std::ptrdiff_t>(count));
  words.insert(words.begin() + static_cast<std::ptrdiff_t>(at), inserted.begin(), inserted.end());
}

/// @brief Consumes one option of `env` at `words[at]`.
/// @param words The words being scanned; an `-S` string is spliced in place of
/// the option, so `at` then names the first spliced word.
/// @param at The option's position; otherwise moved past the option and the
/// argument it takes.
void skip_option(std::vector<std::string>& words, std::size_t& at) {
  std::string const word = words[at];
  auto const        take = [&](std::size_t width, std::string_view inline_value, bool has_inline) {
    // Takes the option's argument: the inline text, else the next word.
    std::string value;
    std::size_t used = width;
    if (has_inline) {
      value = std::string{inline_value};
    } else if (at + width < words.size()) {
      value = words[at + width];
      ++used;
    }
    return std::pair{std::move(value), used};
  };

  if (word.starts_with("--")) {
    auto const equals     = word.find('=');
    auto const name       = word.substr(2, equals == std::string::npos ? std::string::npos : equals - 2);
    bool const inline_arg = equals != std::string::npos;
    auto const value_in   = inline_arg ? std::string_view{word}.substr(equals + 1) : std::string_view{};
    if (name == "unset" || name == "chdir" || name == "argv0") {
      auto const used = take(1, value_in, inline_arg).second;
      at += used;
      return;
    }
    if (name == "split-string") {
      auto [value, used] = take(1, value_in, inline_arg);
      splice_split(words, at, used, value);
      return;
    }
    ++at; // A flag, or an option with only an inline argument.
    return;
  }

  // Short options, possibly clustered: `-iu NAME`, `-uNAME`, `-Sclaude`.
  for (std::size_t j = 1; j < word.size(); ++j) {
    char const c = word[j];
    // `-L` and `-U` are FreeBSD's login-class options, which macOS and GNU env
    // reject. Consuming their value anyway is harmless: such a command fails
    // inside env before anything runs.
    if (c == 'u' || c == 'C' || c == 'P' || c == 'a' || c == 'S' || c == 'L' || c == 'U') {
      bool const has_inline = j + 1 < word.size();
      auto const rest       = std::string_view{word}.substr(j + 1);
      auto [value, used]    = take(1, rest, has_inline);
      if (c == 'S') {
        splice_split(words, at, used, value);
      } else {
        at += used;
      }
      return;
    }
  }
  ++at;
}

} // namespace

auto model_launchers() -> std::span<const std::string_view> {
  return k_launchers;
}

auto check_command(std::span<const std::string> argv) -> std::expected<void, guard_refusal> {
  std::vector<std::string> words(argv.begin(), argv.end());
  std::size_t              at        = 0;
  bool                     after_env = false;
  // Each pass consumes at least one word or splices a finite string, so the
  // loop ends; the cap only bounds a pathological `-S` chain.
  for (int pass = 0; pass < 4096; ++pass) {
    while (at < words.size() && is_assignment(words[at], after_env)) {
      ++at;
    }
    if (at >= words.size()) {
      return {};
    }
    if (!equals_ignore_case(basename_of(words[at]), "env")) {
      break;
    }
    ++at;
    after_env = true;
    while (at < words.size()) {
      auto const& word = words[at];
      if (word == "--") {
        ++at;
        break;
      }
      if (word == "-") {
        ++at;
        continue;
      }
      if (word.size() > 1 && word.front() == '-') {
        skip_option(words, at);
        continue;
      }
      break;
    }
  }
  if (at >= words.size()) {
    return {};
  }
  auto const program = basename_of(words[at]);
  // The refusal names the LISTED spelling, whatever case the command used.
  auto const listed = std::ranges::find_if(k_launchers, [&](std::string_view name) { return equals_ignore_case(program, name); });
  if (listed != k_launchers.end()) {
    return std::unexpected(guard_refusal{.program = std::string{*listed}});
  }
  return {};
}

} // namespace planar::engine::hostqueue

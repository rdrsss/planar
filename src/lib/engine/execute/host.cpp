/// @file host.cpp
/// @brief The Lua half of `planar.engine_execute`: the sandbox, the
/// manifest-driven registrar, the twenty-five host functions, the JSON
/// marshalling in both directions, and the run loop.
///
/// ## This is the ONLY file in the module that includes a Lua header or
/// ## calls `fork`
///
/// Everything here has internal linkage or module linkage; nothing below is
/// exported except the handful of entry points `execute.cppm` declares. In
/// particular `capture_process` — the fork/exec/poll runner — is in the
/// anonymous namespace, so the process-spawning primitive this module needs
/// in order to shell `planar` cannot be named from outside this translation
/// unit, let alone from outside the module. That is the spawn-free
/// invariant's compile-level half; see execute.cppm's header for the other
/// two locks.
///
/// ## Lua raises errors by longjmp, and this file is C++
///
/// Lua 5.5 built as C uses `setjmp`/`longjmp` for `luaL_error`. Longjmping
/// across a frame holding a non-trivially-destructible object is undefined
/// behaviour, so no host function below calls `luaL_error` directly. They
/// `throw host_error` instead, and `guarded()` — the trampoline every
/// registered closure goes through — catches it, copies the message into a
/// plain `char` buffer, lets every C++ frame unwind normally, and only THEN
/// calls `luaL_error` from a frame whose locals are all trivial.
///
/// The inverse hazard (Lua raising through a C++ frame of ours) is narrowed
/// the same way: the object-serialisation loop re-fetches values with
/// `lua_rawget` rather than `lua_getfield`. The two are indistinguishable
/// here — `lua_next` only ever yields keys whose RAW lookup is non-nil, and
/// `__index` is consulted only when the raw lookup misses — so this costs no
/// fidelity and removes the one metamethod call that could have longjmped
/// out from underneath a live `std::vector<std::string>`.
module;

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// lua.hpp, NOT lua.h/lualib.h/lauxlib.h directly: Lua's own headers carry no
// `extern "C"` guard (that is precisely what lua.hpp exists to add), so
// including them from C++ declares every entry point with C++ linkage and
// every call fails to link against the C library — with the linker helpfully
// suggesting "declaration possibly missing 'extern \"C\"'".
#include <lua.hpp>

module planar.engine_execute;

import std;
import planar.json_text;

namespace planar::engine::execute {

namespace {

// ---------------------------------------------------------------------------
// Error plumbing
// ---------------------------------------------------------------------------

/// @brief A host-function failure, on its way to becoming a Lua error.
class host_error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// @brief Throw a formatted `host_error`.
/// @param fmt The format string.
/// @param args The arguments.
template <typename... Args> [[noreturn]] auto fail(std::format_string<Args...> fmt, Args&&... args) -> void {
  throw host_error(std::format(fmt, std::forward<Args>(args)...));
}

/// @brief Run one host-function body, converting any C++ exception into a
/// Lua error raised from a frame with no non-trivial locals.
///
/// See this file's header for why the two-step matters. The message is
/// truncated into a fixed buffer rather than kept in a `std::string`,
/// because `luaL_error` does not return.
/// @param L The Lua state.
/// @param body The host function's real body.
/// @return The body's return count, or never (the error path longjmps).
template <typename Body> auto guarded(lua_State* L, Body body) -> int {
  std::array<char, 768> message{};
  {
    try {
      return body();
    } catch (host_error const& e) {
      std::snprintf(message.data(), message.size(), "%s", e.what());
    } catch (std::exception const& e) {
      std::snprintf(message.data(), message.size(), "%s", e.what());
    } catch (...) {
      std::snprintf(message.data(), message.size(), "planar-execute: unknown host error");
    }
  }
  return luaL_error(L, "%s", message.data());
}

// ---------------------------------------------------------------------------
// Run-scoped host state
// ---------------------------------------------------------------------------

/// @brief Everything the host closures share, captured as upvalue #1 of
/// every registered function.
///
/// One instance per run. Not thread-safe, and never needs to be: the engine
/// is single-threaded by design — one clean process per phase, no scheduler.
struct host_state {
  run_config const* config = nullptr;
  std::ostream*     err    = nullptr;

  std::optional<std::string> result_json; ///< Captured `flow.result` payload.
  std::optional<std::string> fail_msg;    ///< Captured `flow.fail` message.
  std::string                current_phase;
};

/// @brief Recover the `host_state` from C-closure upvalue #1.
/// @param L The Lua state.
/// @return The state.
auto upvalue_state(lua_State* L) -> host_state& {
  auto* raw = lua_touserdata(L, lua_upvalueindex(1));
  return *static_cast<host_state*>(raw);
}

// ---------------------------------------------------------------------------
// Lua argument helpers
// ---------------------------------------------------------------------------

/// @brief Read the string at `idx`, or the empty string when it is not one.
///
/// Deliberately NOT `luaL_checkstring`: that coerces numbers and raises its
/// own Lua error with a different message shape. The oracle's helper returns
/// "" for a non-string and lets the caller's own validator produce the
/// diagnostic, which is what keeps `git.checkout(42)` reporting "unsafe ref"
/// rather than a type error.
/// @param L The Lua state.
/// @param idx The stack index.
/// @return The string view (valid while the value stays on the stack).
auto arg_string(lua_State* L, int idx) -> std::string_view {
  if (lua_type(L, idx) != LUA_TSTRING) {
    return {};
  }
  std::size_t len = 0;
  char const* raw = lua_tolstring(L, idx, &len);
  return raw == nullptr ? std::string_view{} : std::string_view{raw, len};
}

/// @brief Read a Lua array-table of strings into a vector.
/// @param L The Lua state.
/// @param idx The stack index of the table.
/// @return The argv.
auto argv_from_table(lua_State* L, int idx) -> std::vector<std::string> {
  int const abs = lua_absindex(L, idx);
  if (lua_type(L, abs) != LUA_TTABLE) {
    fail("expected an argv table (array of strings)");
  }
  auto const               count = static_cast<std::size_t>(lua_rawlen(L, abs));
  std::vector<std::string> argv;
  argv.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    lua_rawgeti(L, abs, static_cast<lua_Integer>(i + 1)); // Lua arrays are 1-based.
    if (lua_type(L, -1) != LUA_TSTRING) {
      lua_settop(L, -2);
      fail("argv element {} is not a string", i + 1);
    }
    std::size_t len = 0;
    char const* raw = lua_tolstring(L, -1, &len);
    argv.emplace_back(raw, len);
    lua_settop(L, -2);
  }
  return argv;
}

/// @brief Read an integer field from the table at `idx`.
///
/// Port target: `host.zig`'s `tableIntField`. Pops the field off the stack
/// before returning, whether or not it was found.
/// @param L The Lua state.
/// @param idx The table's stack index.
/// @param field The field name.
/// @return The integer, or unset when absent or not an integer.
auto table_int_field(lua_State* L, int idx, char const* field) -> std::optional<std::int64_t> {
  lua_getfield(L, idx, field);
  std::optional<std::int64_t> out;
  if (lua_isinteger(L, -1) != 0) {
    out = static_cast<std::int64_t>(lua_tointeger(L, -1));
  }
  lua_settop(L, -2);
  return out;
}

/// @brief Read a string field from the table at `idx`, copied off the
/// stack.
///
/// Port target: `host.zig`'s `tableStrField`. The Zig original dupes onto an
/// arena so the value survives the field's stack slot being popped; the
/// C++ `std::string` copy below is the same fix by construction.
/// @param L The Lua state.
/// @param idx The table's stack index.
/// @param field The field name.
/// @return The string, or unset when absent or not a string.
auto table_str_field(lua_State* L, int idx, char const* field) -> std::optional<std::string> {
  lua_getfield(L, idx, field);
  std::optional<std::string> out;
  if (lua_type(L, -1) == LUA_TSTRING) {
    std::size_t len = 0;
    char const* raw = lua_tolstring(L, -1, &len);
    out             = std::string{raw, len};
  }
  lua_settop(L, -2);
  return out;
}

/// @brief Trim ASCII whitespace from both ends.
/// @param text The input.
/// @return The trimmed view.
auto trim(std::string_view text) -> std::string_view {
  constexpr std::string_view ws    = " \t\r\n";
  auto const                 first = text.find_first_not_of(ws);
  if (first == std::string_view::npos) {
    return {};
  }
  return text.substr(first, text.find_last_not_of(ws) - first + 1);
}

// ---------------------------------------------------------------------------
// JSON text -> Lua
// ---------------------------------------------------------------------------

/// @brief A recursive-descent JSON reader that pushes straight onto the Lua
/// stack.
///
/// ## Why this is hand-written instead of `glz::generic`
///
/// Because Glaze's generic value stores every number as a `double`, and this
/// path is where a planner read's integers enter a workflow and later leave
/// it again through `flow.result`. Round-tripping through a double is
/// OBSERVABLE, and was verified against the oracle rather than assumed:
///
///     --args '{"n":5,"big":9007199254740993}'
///     oracle: {"big":9007199254740993,"n":5}   math.type(n) == "integer"
///
/// A `double` intermediate turns `n` into `5.0` (Zig's `{d}` prints it `5`,
/// but `math.type` reports `float`, and any arithmetic a workflow does with
/// it changes shape), and turns `big` into 9007199254740992 — a silently
/// wrong id. Parsing straight to the stack also skips the intermediate tree
/// entirely, which is why it is barely longer than the Glaze call would be.
class json_reader {
public:
  /// @brief Construct over the document text.
  /// @param text The JSON.
  explicit json_reader(std::string_view text) : _text(text) {
  }

  /// @brief Parse the whole document and leave exactly one value on the
  /// stack.
  /// @param L The Lua state.
  auto parse_into(lua_State* L) -> void {
    skip_ws();
    push_value(L, 0);
    skip_ws();
    if (_pos != _text.size()) {
      fail("host returned non-JSON output");
    }
  }

private:
  std::string_view _text;
  std::size_t      _pos = 0;

  auto skip_ws() -> void {
    while (_pos < _text.size() && (_text[_pos] == ' ' || _text[_pos] == '\t' || _text[_pos] == '\n' || _text[_pos] == '\r')) {
      ++_pos;
    }
  }

  auto peek() const -> char {
    if (_pos >= _text.size()) {
      fail("host returned non-JSON output");
    }
    return _text[_pos];
  }

  auto expect(std::string_view literal) -> void {
    if (_text.substr(_pos, literal.size()) != literal) {
      fail("host returned non-JSON output");
    }
    _pos += literal.size();
  }

  auto push_value(lua_State* L, int depth) -> void {
    // A depth cap, because the document is untrusted (it is whatever a
    // shelled binary printed) and this recursion is on the C stack.
    if (depth > 200) {
      fail("host returned JSON nested too deeply");
    }
    if (lua_checkstack(L, 4) == 0) {
      fail("out of Lua stack decoding host JSON");
    }
    switch (peek()) {
    case '{':
      push_object(L, depth);
      return;
    case '[':
      push_array(L, depth);
      return;
    case '"':
      push_string(L);
      return;
    case 't':
      expect("true");
      lua_pushboolean(L, 1);
      return;
    case 'f':
      expect("false");
      lua_pushboolean(L, 0);
      return;
    case 'n':
      expect("null");
      lua_pushnil(L);
      return;
    default:
      push_number(L);
      return;
    }
  }

  auto push_object(lua_State* L, int depth) -> void {
    ++_pos; // '{'
    lua_createtable(L, 0, 0);
    skip_ws();
    if (peek() == '}') {
      ++_pos;
      return;
    }
    while (true) {
      skip_ws();
      if (peek() != '"') {
        fail("host returned non-JSON output");
      }
      push_string(L);
      skip_ws();
      if (peek() != ':') {
        fail("host returned non-JSON output");
      }
      ++_pos;
      skip_ws();
      push_value(L, depth + 1);
      // A JSON null value would push nil, which erases the key rather than
      // storing it — exactly what the oracle does (its pushJsonValue maps
      // .null to lua_pushnil and lua_settable then removes the field), so a
      // null field is ABSENT in Lua rather than present-and-nil.
      lua_settable(L, -3);
      skip_ws();
      if (peek() == ',') {
        ++_pos;
        continue;
      }
      if (peek() == '}') {
        ++_pos;
        return;
      }
      fail("host returned non-JSON output");
    }
  }

  auto push_array(lua_State* L, int depth) -> void {
    ++_pos; // '['
    lua_createtable(L, 0, 0);
    skip_ws();
    if (peek() == ']') {
      ++_pos;
      return;
    }
    lua_Integer index = 1;
    while (true) {
      skip_ws();
      push_value(L, depth + 1);
      lua_rawseti(L, -2, index);
      ++index;
      skip_ws();
      if (peek() == ',') {
        ++_pos;
        continue;
      }
      if (peek() == ']') {
        ++_pos;
        return;
      }
      fail("host returned non-JSON output");
    }
  }

  auto push_string(lua_State* L) -> void {
    std::string const value = read_string();
    lua_pushlstring(L, value.data(), value.size());
  }

  auto read_string() -> std::string {
    if (peek() != '"') {
      fail("host returned non-JSON output");
    }
    ++_pos;
    std::string out;
    while (true) {
      if (_pos >= _text.size()) {
        fail("host returned non-JSON output");
      }
      char const c = _text[_pos++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (_pos >= _text.size()) {
        fail("host returned non-JSON output");
      }
      switch (char const esc = _text[_pos++]) {
      case '"':
        out.push_back('"');
        break;
      case '\\':
        out.push_back('\\');
        break;
      case '/':
        out.push_back('/');
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u':
        append_utf8(out, read_unicode_escape());
        break;
      default:
        (void)esc;
        fail("host returned non-JSON output");
      }
    }
  }

  auto read_hex4() -> std::uint32_t {
    if (_pos + 4 > _text.size()) {
      fail("host returned non-JSON output");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      char const    c     = _text[_pos++];
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        fail("host returned non-JSON output");
      }
      value = (value << 4) | digit;
    }
    return value;
  }

  auto read_unicode_escape() -> std::uint32_t {
    std::uint32_t const first = read_hex4();
    if (first < 0xD800 || first > 0xDBFF) {
      return first;
    }
    // High surrogate: a low surrogate must follow, or the text is malformed.
    if (_text.substr(_pos, 2) != "\\u") {
      fail("host returned non-JSON output");
    }
    _pos += 2;
    std::uint32_t const second = read_hex4();
    if (second < 0xDC00 || second > 0xDFFF) {
      fail("host returned non-JSON output");
    }
    return 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
  }

  static auto append_utf8(std::string& out, std::uint32_t code) -> void {
    if (code < 0x80) {
      out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (code >> 6)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (code >> 12)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (code >> 18)));
      out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
  }

  auto push_number(lua_State* L) -> void {
    std::size_t const start = _pos;
    if (_pos < _text.size() && (_text[_pos] == '-' || _text[_pos] == '+')) {
      ++_pos;
    }
    bool floating = false;
    while (_pos < _text.size()) {
      char const c = _text[_pos];
      if (c >= '0' && c <= '9') {
        ++_pos;
      } else if (c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
        floating = floating || (c == '.' || c == 'e' || c == 'E');
        ++_pos;
      } else {
        break;
      }
    }
    std::string_view const token = _text.substr(start, _pos - start);
    if (token.empty()) {
      fail("host returned non-JSON output");
    }
    if (!floating) {
      std::int64_t as_int  = 0;
      auto const [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), as_int);
      if (ec == std::errc{} && ptr == token.data() + token.size()) {
        lua_pushinteger(L, static_cast<lua_Integer>(as_int));
        return;
      }
      // Falls through to the float path only on int64 OVERFLOW, which is the
      // one case where precision has to be given up anyway.
    }
    double as_double     = 0;
    auto const [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), as_double);
    if (ec != std::errc{} || ptr != token.data() + token.size()) {
      fail("host returned non-JSON output");
    }
    lua_pushnumber(L, as_double);
  }
};

/// @brief Parse `json` and push it; an empty document pushes an empty table.
/// @param L The Lua state.
/// @param json The document.
auto push_parsed_json(lua_State* L, std::string_view json) -> void {
  auto const trimmed = trim(json);
  if (trimmed.empty()) {
    lua_createtable(L, 0, 0);
    return;
  }
  json_reader{trimmed}.parse_into(L);
}

// ---------------------------------------------------------------------------
// Lua -> JSON text
// ---------------------------------------------------------------------------

auto write_lua_json(lua_State* L, int idx, std::string& out, int depth) -> void;

/// @brief Serialise the table at `idx`.
///
/// Array-vs-object is decided by `lua_rawlen`: a non-empty sequence becomes
/// a JSON array (and must be dense — a hole is an error, not a silent
/// truncation), anything else becomes an object. An empty table becomes
/// `{}`, matching the oracle; `[]` would be equally defensible and is not
/// what it emits.
///
/// OBJECT KEYS ARE SORTED BYTEWISE, and that is a contract rather than
/// tidiness. Lua's `lua_next` order over a string-keyed table depends on the
/// per-state hash seed, which `luaL_newstate` derives from time and an ASLR
/// address — so raw traversal order is not reproducible across runs of the
/// SAME binary on the same input. A deterministic engine whose output moves
/// run to run is not deterministic. Verified against the oracle, whose
/// output for a forty-key table comes back alphabetical.
auto write_lua_table_json(lua_State* L, int idx, std::string& out, int depth) -> void {
  // Absolutise first: every push below shifts the stack, and a relative
  // index would then point at the wrong slot.
  int const  abs = lua_absindex(L, idx);
  auto const len = static_cast<std::size_t>(lua_rawlen(L, abs));

  if (len > 0) {
    // Validate density before emitting anything, so a sparse table fails
    // rather than producing a half-written array.
    std::size_t key_count = 0;
    lua_pushnil(L);
    while (lua_next(L, abs) != 0) {
      ++key_count;
      bool const        integral = lua_isinteger(L, -2) != 0;
      lua_Integer const key      = integral ? lua_tointeger(L, -2) : 0;
      lua_settop(L, -2); // pop the value, keep the key for lua_next
      if (!integral) {
        lua_settop(L, -2);
        fail("mixed Lua tables cannot be serialized as JSON arrays");
      }
      if (key < 1 || static_cast<std::size_t>(key) > len) {
        lua_settop(L, -2);
        fail("sparse Lua arrays are not supported");
      }
    }
    if (key_count != len) {
      fail("sparse Lua arrays are not supported");
    }
    out.push_back('[');
    for (std::size_t i = 1; i <= len; ++i) {
      if (i > 1) {
        out.push_back(',');
      }
      lua_rawgeti(L, abs, static_cast<lua_Integer>(i));
      write_lua_json(L, -1, out, depth + 1);
      lua_settop(L, -2);
    }
    out.push_back(']');
    return;
  }

  std::vector<std::string> keys;
  lua_pushnil(L);
  while (lua_next(L, abs) != 0) {
    if (lua_type(L, -2) != LUA_TSTRING) {
      lua_settop(L, -3);
      fail("JSON object key must be a string");
    }
    std::size_t klen = 0;
    char const* kraw = lua_tolstring(L, -2, &klen);
    keys.emplace_back(kraw, klen);
    lua_settop(L, -2);
  }
  std::ranges::sort(keys);

  out.push_back('{');
  bool first = true;
  for (auto const& key : keys) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    json_text::append_json_string(out, key);
    out.push_back(':');
    lua_pushlstring(L, key.data(), key.size());
    lua_rawget(L, abs); // rawget, not getfield — see this file's header.
    write_lua_json(L, -1, out, depth + 1);
    lua_settop(L, -2);
  }
  out.push_back('}');
}

auto write_lua_json(lua_State* L, int idx, std::string& out, int depth) -> void {
  if (depth > 200) {
    fail("cannot serialize a Lua value nested that deeply");
  }
  if (lua_checkstack(L, 4) == 0) {
    fail("out of Lua stack serializing result");
  }
  int const abs = lua_absindex(L, idx);
  switch (lua_type(L, abs)) {
  case LUA_TNIL:
    out += "null";
    return;
  case LUA_TBOOLEAN:
    out += (lua_toboolean(L, abs) != 0) ? "true" : "false";
    return;
  case LUA_TNUMBER:
    if (lua_isinteger(L, abs) != 0) {
      out += std::format("{}", static_cast<std::int64_t>(lua_tointeger(L, abs)));
    } else {
      out += format_double(static_cast<double>(lua_tonumber(L, abs)));
    }
    return;
  case LUA_TSTRING: {
    std::size_t len = 0;
    char const* raw = lua_tolstring(L, abs, &len);
    json_text::append_json_string(out, std::string_view{raw, len});
    return;
  }
  case LUA_TTABLE:
    write_lua_table_json(L, abs, out, depth);
    return;
  default:
    fail("cannot serialize Lua {} to JSON", lua_typename(L, lua_type(L, abs)));
  }
}

/// @brief Serialise the value at `idx` to JSON text.
/// @param L The Lua state.
/// @param idx The stack index.
/// @return The JSON.
auto lua_to_json(lua_State* L, int idx) -> std::string {
  std::string out;
  write_lua_json(L, lua_absindex(L, idx), out, 0);
  return out;
}

// ---------------------------------------------------------------------------
// Subprocess capture — INTERNAL LINKAGE, and that is the point
// ---------------------------------------------------------------------------

/// @brief One finished subprocess.
struct process_result {
  int         code = -1;
  std::string out;
  std::string err;
};

/// @brief Read all of `fd` in one blocking gulp, appending to `sink`, until
/// EOF or `limit` bytes.
/// @param fd The descriptor.
/// @param sink The destination.
/// @param limit The cap.
/// @return `false` when the descriptor is exhausted.
auto drain_once(int fd, std::string& sink, std::size_t limit) -> bool {
  std::array<char, 8192> buffer{};
  auto const             got = ::read(fd, buffer.data(), buffer.size());
  if (got <= 0) {
    return got < 0 && (errno == EINTR || errno == EAGAIN);
  }
  auto const room = limit > sink.size() ? limit - sink.size() : 0;
  sink.append(buffer.data(), std::min(static_cast<std::size_t>(got), room));
  return true;
}

/// @brief Run `argv[0]` with `argv`, capturing both output streams.
///
/// Both pipes are polled together. Reading stdout to EOF and only then
/// draining stderr — the shorter spelling — deadlocks the moment a child
/// writes more than a pipe buffer of diagnostics before finishing its
/// payload, which `planar <verb> --json` on a warning path does.
///
/// NOTHING outside this translation unit can call this. There is no exported
/// declaration and no header; see this file's header comment.
///
/// `search_path` picks the exec flavour, and the choice is load-bearing:
///
///   - `false` (the default) execs `argv[0]` verbatim, so the caller's
///     resolution is the only resolution. The `cli.*` group depends on this:
///     it resolves an allowlisted Planar binary as a SIBLING of this
///     executable, and a `PATH` search there would let an unrelated `planar`
///     earlier on `PATH` answer for the one that shipped with this
///     planar-execute. Never pass `true` for `cli.*`.
///   - `true` searches `PATH` the way the shell would. Only `git.*` uses it,
///     matching the oracle, which spawns the bare name `git` and lets Zig's
///     `std.process.Child` resolve it (planar-execute/host.zig `runGit`).
///     Probing a fixed list of absolute paths instead — as this did — fails
///     outright on a machine whose git lives somewhere unlisted, e.g. only in
///     `/opt/homebrew/bin`, because the bare-name `execv` fallback does NOT
///     search `PATH`.
/// @param argv The full argument vector, argv[0] being the executable path.
/// @param search_path Whether to resolve argv[0] through `PATH`.
/// @return The captured result, or unset when the child could not start.
auto capture_process(std::vector<std::string> const& argv, bool search_path = false) -> std::optional<process_result> {
  std::array<int, 2> out_pipe{-1, -1};
  std::array<int, 2> err_pipe{-1, -1};
  if (::pipe(out_pipe.data()) != 0) {
    return std::nullopt;
  }
  if (::pipe(err_pipe.data()) != 0) {
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    return std::nullopt;
  }

  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (auto const& arg : argv) {
    raw.push_back(const_cast<char*>(arg.c_str()));
  }
  raw.push_back(nullptr);

  pid_t const pid = ::fork();
  if (pid < 0) {
    for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
      ::close(fd);
    }
    return std::nullopt;
  }
  if (pid == 0) {
    // Child. Only async-signal-safe calls from here to exec. Both execv and
    // execvp are async-signal-safe.
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    if (search_path) {
      ::execvp(raw[0], raw.data());
    } else {
      ::execv(raw[0], raw.data());
    }
    ::_exit(127);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);

  process_result        result;
  constexpr std::size_t k_stdout_limit = 8U * 1024U * 1024U;
  constexpr std::size_t k_stderr_limit = 64U * 1024U;
  std::array<pollfd, 2> fds{pollfd{.fd = out_pipe[0], .events = POLLIN, .revents = 0},
                            pollfd{.fd = err_pipe[0], .events = POLLIN, .revents = 0}};
  while (fds[0].fd >= 0 || fds[1].fd >= 0) {
    if (::poll(fds.data(), fds.size(), -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    for (std::size_t i = 0; i < fds.size(); ++i) {
      if (fds[i].fd < 0 || fds[i].revents == 0) {
        continue;
      }
      auto& sink  = (i == 0) ? result.out : result.err;
      auto  limit = (i == 0) ? k_stdout_limit : k_stderr_limit;
      if (!drain_once(fds[i].fd, sink, limit)) {
        ::close(fds[i].fd);
        fds[i].fd = -1;
      }
    }
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    // Retry.
  }
  result.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return result;
}

/// @brief Shell an allowlisted Planar binary and return its stdout.
///
/// Two gates, not one. The binary must be in `allowed_cli_bins()`, AND the
/// command must pass `command_allowed()` — being permitted to run `planar`
/// is not being permitted to run every `planar` verb. The binary is then
/// resolved as a SIBLING of this executable rather than through `PATH`, so
/// what runs is the planar that shipped with this planar-execute.
/// @param hs The host state.
/// @param bin The binary name.
/// @param argv_tail The arguments.
/// @return The child's stdout.
auto run_allowlisted(host_state& hs, std::string_view bin, std::vector<std::string> const& argv_tail) -> std::string {
  auto const bins = allowed_cli_bins();
  if (std::ranges::find(bins, bin) == bins.end()) {
    fail("binary not allowlisted: {}", bin);
  }
  if (!command_allowed(bin, argv_tail)) {
    fail("command is outside the deterministic workflow capability set");
  }
  if (hs.config->bin_dir.empty()) {
    fail("trusted binary directory is unavailable");
  }

  std::vector<std::string> argv;
  argv.reserve(argv_tail.size() + 1);
  argv.push_back((std::filesystem::path{hs.config->bin_dir} / std::string{bin}).string());
  argv.insert(argv.end(), argv_tail.begin(), argv_tail.end());

  auto const result = capture_process(argv);
  if (!result.has_value()) {
    fail("failed to spawn {}", bin);
  }
  if (result->code != 0) {
    fail("{} exited non-zero: {}", bin, trim(result->err));
  }
  return result->out;
}

/// @brief Shell `git -C <worktree> <args...>`.
///
/// The `-C` is injected by the host from the run config; a workflow never
/// names the directory and has no way to point git somewhere else.
///
/// git is spawned by BARE NAME through `PATH`, matching the oracle
/// (planar-execute/host.zig `runGit` sets `argv[0] = "git"` and lets Zig
/// resolve it). This is deliberately unlike the `cli.*` group, which resolves
/// beside this binary and must never search `PATH`; see `capture_process`.
/// @param hs The host state.
/// @param git_args The git arguments.
/// @return The child's stdout.
auto run_git(host_state& hs, std::vector<std::string> const& git_args) -> std::string {
  if (hs.config->worktree.empty()) {
    fail("git.* requires a configured worktree (--worktree)");
  }
  std::vector<std::string> argv{"git", "-C", hs.config->worktree};
  argv.insert(argv.end(), git_args.begin(), git_args.end());
  auto const result = capture_process(argv, /*search_path=*/true);
  if (!result.has_value()) {
    fail("failed to spawn git");
  }
  if (result->code != 0) {
    fail("git exited non-zero: {}", trim(result->err));
  }
  return result->out;
}

// ---------------------------------------------------------------------------
// fs confinement
// ---------------------------------------------------------------------------

/// @brief An open directory descriptor plus the leaf name to operate on.
class confined_parent {
public:
  confined_parent() = default;
  confined_parent(int fd, std::string leaf) : _fd(fd), _leaf(std::move(leaf)) {
  }
  confined_parent(confined_parent const&)                    = delete;
  auto operator=(confined_parent const&) -> confined_parent& = delete;
  confined_parent(confined_parent&& other) noexcept : _fd(std::exchange(other._fd, -1)), _leaf(std::move(other._leaf)) {
  }
  auto operator=(confined_parent&& other) noexcept -> confined_parent& {
    if (this != &other) {
      reset();
      _fd   = std::exchange(other._fd, -1);
      _leaf = std::move(other._leaf);
    }
    return *this;
  }
  ~confined_parent() {
    reset();
  }

  /// @brief The directory descriptor.
  /// @return The fd.
  [[nodiscard]] auto fd() const -> int {
    return _fd;
  }
  /// @brief The final path component.
  /// @return The leaf name.
  [[nodiscard]] auto leaf() const -> std::string const& {
    return _leaf;
  }

private:
  auto reset() -> void {
    if (_fd >= 0) {
      ::close(_fd);
      _fd = -1;
    }
  }
  int         _fd = -1;
  std::string _leaf;
};

/// @brief Why a confined open failed.
enum class confine_error : std::uint8_t { rejected, not_found };

/// @brief Walk from the sandbox root to `rel`'s parent directory, opening
/// every component with `O_NOFOLLOW`.
///
/// The component-by-component no-follow walk is the half that actually
/// confines. `validate_confined_rel` rejects `..` in the TEXT, which stops
/// the obvious escape; it does nothing about a symlink placed at
/// `sub/dir` pointing at `/`. Opening each component with `O_NOFOLLOW` does,
/// and it does so even if the symlink appears between the check and the
/// open, because there is no path resolution left for the kernel to redo.
/// @param sandbox_root The root; empty is an error.
/// @param rel The sandbox-relative path.
/// @param create_parents Whether to create missing intermediate directories.
/// @return The open parent, or why it failed.
auto open_confined_parent(std::string_view sandbox_root, std::string_view rel, bool create_parents)
    -> std::expected<confined_parent, confine_error> {
  if (sandbox_root.empty() || !validate_confined_rel(rel)) {
    return std::unexpected(confine_error::rejected);
  }
  std::string const root{sandbox_root};
  int               current = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (current < 0) {
    return std::unexpected(errno == ENOENT ? confine_error::not_found : confine_error::rejected);
  }

  std::vector<std::string> components;
  for (auto const part : std::views::split(rel, '/')) {
    components.emplace_back(std::string_view{part.begin(), part.end()});
  }
  std::string const leaf = components.back();
  components.pop_back();

  for (auto const& component : components) {
    int next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (next < 0 && errno == ENOENT && create_parents) {
      ::mkdirat(current, component.c_str(), 0755);
      next = ::openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    }
    if (next < 0) {
      auto const why = (errno == ENOENT) ? confine_error::not_found : confine_error::rejected;
      ::close(current);
      return std::unexpected(why);
    }
    ::close(current);
    current = next;
  }
  return confined_parent{current, leaf};
}

// ---------------------------------------------------------------------------
// Host functions — cli.*
// ---------------------------------------------------------------------------

/// @brief Body shared by the six `cli.*` shells.
/// @param L The Lua state.
/// @param bin Which allowlisted binary.
/// @param as_json Whether to parse stdout as JSON.
/// @return The Lua return count (always 1).
auto cli_shell(lua_State* L, std::string_view bin, bool as_json) -> int {
  auto&       hs   = upvalue_state(L);
  auto const  argv = argv_from_table(L, 1);
  std::string out  = run_allowlisted(hs, bin, argv);
  if (as_json) {
    push_parsed_json(L, out);
  } else {
    lua_pushlstring(L, out.data(), out.size());
  }
  return 1;
}

extern "C" auto host_cli_planar(lua_State* L) -> int {
  return guarded(L, [L] { return cli_shell(L, "planar", false); });
}
extern "C" auto host_cli_planar_json(lua_State* L) -> int {
  return guarded(L, [L] { return cli_shell(L, "planar", true); });
}
extern "C" auto host_cli_planar_agent(lua_State* L) -> int {
  return guarded(L, [L] { return cli_shell(L, "planar-agent", false); });
}
extern "C" auto host_cli_planar_agent_json(lua_State* L) -> int {
  return guarded(L, [L] { return cli_shell(L, "planar-agent", true); });
}
extern "C" auto host_cli_planar_watch(lua_State* L) -> int {
  return guarded(L, [L] { return cli_shell(L, "planar-watch", false); });
}
extern "C" auto host_cli_planar_watch_json(lua_State* L) -> int {
  return guarded(L, [L] { return cli_shell(L, "planar-watch", true); });
}

// ---------------------------------------------------------------------------
// Host functions — git.*
// ---------------------------------------------------------------------------

extern "C" auto host_git_reset_hard(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs  = upvalue_state(L);
    auto const sha = arg_string(L, 1);
    if (!is_hex_object_id(sha)) {
      fail("git.reset_hard requires a hexadecimal object id");
    }
    run_git(hs, {"reset", "--hard", std::string{sha}});
    return 0;
  });
}

extern "C" auto host_git_checkout(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs  = upvalue_state(L);
    auto const ref = arg_string(L, 1);
    if (!safe_git_ref(ref)) {
      fail("git.checkout received an unsafe ref");
    }
    run_git(hs, {"checkout", std::string{ref}});
    return 0;
  });
}

extern "C" auto host_git_diff_name_only(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&       hs = upvalue_state(L);
    std::string base;
    if (lua_type(L, 1) == LUA_TTABLE) {
      lua_getfield(L, 1, "base");
      base = std::string{arg_string(L, -1)};
      lua_settop(L, -2);
    }
    if (!base.empty() && !safe_git_ref(base)) {
      fail("git.diff_name_only received an unsafe base");
    }
    std::string const out =
        base.empty() ? run_git(hs, {"diff", "--name-only"}) : run_git(hs, {"diff", "--name-only", base, "--"});
    lua_createtable(L, 0, 0);
    lua_Integer index = 1;
    for (auto const line : std::views::split(std::string_view{out}, '\n')) {
      auto const entry = trim(std::string_view{line.begin(), line.end()});
      if (entry.empty()) {
        continue;
      }
      lua_pushlstring(L, entry.data(), entry.size());
      lua_rawseti(L, -2, index);
      ++index;
    }
    return 1;
  });
}

extern "C" auto host_git_head_sha(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs  = upvalue_state(L);
    auto const out = run_git(hs, {"rev-parse", "HEAD"});
    auto const sha = trim(out);
    lua_pushlstring(L, sha.data(), sha.size());
    return 1;
  });
}

extern "C" auto host_git_clean(lua_State* L) -> int {
  return guarded(L, [L] {
    auto& hs          = upvalue_state(L);
    bool  directories = false;
    if (lua_type(L, 1) == LUA_TTABLE) {
      lua_getfield(L, 1, "directories");
      directories = lua_toboolean(L, -1) != 0;
      lua_settop(L, -2);
    }
    // `-f` always: git refuses to clean without it, so the flag is not a
    // policy choice this function gets to make.
    run_git(hs, {"clean", directories ? "-fd" : "-f"});
    return 0;
  });
}

// ---------------------------------------------------------------------------
// Host functions — fs.*
// ---------------------------------------------------------------------------

extern "C" auto host_fs_read(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs     = upvalue_state(L);
    auto const rel    = std::string{arg_string(L, 1)};
    auto       parent = open_confined_parent(hs.config->sandbox_root, rel, false);
    if (!parent.has_value()) {
      fail("fs.read rejected or failed: {}", rel);
    }
    int const fd = ::openat(parent->fd(), parent->leaf().c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
      fail("fs.read rejected or failed: {}", rel);
    }
    std::string             data;
    constexpr std::size_t   k_limit = 16U * 1024U * 1024U;
    std::array<char, 65536> buffer{};
    while (true) {
      auto const got = ::read(fd, buffer.data(), buffer.size());
      if (got < 0) {
        if (errno == EINTR) {
          continue;
        }
        ::close(fd);
        fail("fs.read failed: {}", rel);
      }
      if (got == 0) {
        break;
      }
      if (data.size() + static_cast<std::size_t>(got) > k_limit) {
        ::close(fd);
        fail("fs.read failed: {}", rel);
      }
      data.append(buffer.data(), static_cast<std::size_t>(got));
    }
    ::close(fd);
    lua_pushlstring(L, data.data(), data.size());
    return 1;
  });
}

extern "C" auto host_fs_write(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs     = upvalue_state(L);
    auto const rel    = std::string{arg_string(L, 1)};
    auto const data   = std::string{arg_string(L, 2)};
    auto       parent = open_confined_parent(hs.config->sandbox_root, rel, true);
    if (!parent.has_value()) {
      fail("fs.write rejected or failed: {}", rel);
    }
    int fd = ::openat(parent->fd(), parent->leaf().c_str(), O_WRONLY | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
      fd = ::openat(parent->fd(), parent->leaf().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    }
    if (fd < 0) {
      fail("fs.write rejected or failed: {}", rel);
    }
    std::size_t written = 0;
    while (written < data.size()) {
      auto const put = ::write(fd, data.data() + written, data.size() - written);
      if (put < 0) {
        if (errno == EINTR) {
          continue;
        }
        ::close(fd);
        fail("fs.write failed: {}", rel);
      }
      written += static_cast<std::size_t>(put);
    }
    // Truncate: an existing longer file would otherwise keep its tail, which
    // turns "write these bytes" into "overwrite the prefix".
    if (::ftruncate(fd, static_cast<off_t>(data.size())) != 0) {
      ::close(fd);
      fail("fs.write failed: {}", rel);
    }
    ::close(fd);
    return 0;
  });
}

extern "C" auto host_fs_exists(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs     = upvalue_state(L);
    auto const rel    = std::string{arg_string(L, 1)};
    auto       parent = open_confined_parent(hs.config->sandbox_root, rel, false);
    if (!parent.has_value()) {
      if (parent.error() == confine_error::not_found) {
        lua_pushboolean(L, 0);
        return 1;
      }
      fail("fs.exists rejected or failed: {}", rel);
    }
    struct stat info{};
    bool        exists = false;
    if (::fstatat(parent->fd(), parent->leaf().c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) {
      // A symlink reports FALSE even when it resolves: the sandbox does not
      // follow links, so a link is not something `fs.read` could open.
      exists = !S_ISLNK(info.st_mode);
    } else if (errno != ENOENT) {
      fail("fs.exists failed: {}", rel);
    }
    lua_pushboolean(L, exists ? 1 : 0);
    return 1;
  });
}

extern "C" auto host_fs_mkdir(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs     = upvalue_state(L);
    auto const rel    = std::string{arg_string(L, 1)};
    auto       parent = open_confined_parent(hs.config->sandbox_root, rel, true);
    if (!parent.has_value()) {
      fail("fs.mkdir rejected or failed: {}", rel);
    }
    if (::mkdirat(parent->fd(), parent->leaf().c_str(), 0755) != 0 && errno != EEXIST) {
      fail("fs.mkdir failed: {}", rel);
    }
    // Re-open no-follow to prove what is there is a real directory and not a
    // pre-existing symlink that `mkdirat` bounced off with EEXIST.
    int const check = ::openat(parent->fd(), parent->leaf().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (check < 0) {
      fail("fs.mkdir rejected symlink: {}", rel);
    }
    ::close(check);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// Host functions — flow.*
// ---------------------------------------------------------------------------

extern "C" auto host_flow_log(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs  = upvalue_state(L);
    auto const msg = arg_string(L, 1);
    // stderr, so stdout stays a clean JSON channel.
    *hs.err << "[planar-execute] " << msg << '\n';
    hs.err->flush();
    return 0;
  });
}

extern "C" auto host_flow_phase(lua_State* L) -> int {
  return guarded(L, [L] {
    auto& hs         = upvalue_state(L);
    hs.current_phase = std::string{arg_string(L, 1)};
    return 0;
  });
}

extern "C" auto host_flow_fail(lua_State* L) -> int {
  return guarded(L, [L] {
    auto&      hs  = upvalue_state(L);
    auto const msg = std::string{arg_string(L, 1)};
    hs.fail_msg    = msg;
    // Raises as well as records: the phase must stop here, and the run loop
    // prefers the recorded message over the Lua error text when reporting.
    fail("flow.fail: {}", msg);
    return 0;
  });
}

extern "C" auto host_flow_result(lua_State* L) -> int {
  return guarded(L, [L] {
    auto& hs = upvalue_state(L);
    if (lua_type(L, 1) != LUA_TTABLE) {
      fail("flow.result expects a table");
    }
    hs.result_json = lua_to_json(L, 1);
    return 0;
  });
}

// ---------------------------------------------------------------------------
// Host functions — ctx.*
// ---------------------------------------------------------------------------

/// @brief Body shared by the four `ctx.*` planner reads: read a positive
/// integer id, shell a fixed `planar` command, push the parsed JSON.
/// @param L The Lua state.
/// @param who The function's name, for diagnostics.
/// @param verbs The command tokens preceding the id.
/// @return The Lua return count (always 1).
auto ctx_read(lua_State* L, std::string_view who, std::vector<std::string> verbs) -> int {
  auto& hs = upvalue_state(L);
  if (lua_isinteger(L, 1) == 0) {
    fail("{}: expected an integer id", who);
  }
  auto const id = static_cast<std::int64_t>(lua_tointeger(L, 1));
  if (id <= 0) {
    fail("{}: id must be positive", who);
  }
  verbs.push_back(std::format("{}", id));
  verbs.emplace_back("--json");
  push_parsed_json(L, run_allowlisted(hs, "planar", verbs));
  return 1;
}

extern "C" auto host_ctx_plan_show(lua_State* L) -> int {
  return guarded(L, [L] { return ctx_read(L, "ctx.plan_show", {"plan", "show"}); });
}
extern "C" auto host_ctx_task_show(lua_State* L) -> int {
  return guarded(L, [L] { return ctx_read(L, "ctx.task_show", {"task", "show"}); });
}
extern "C" auto host_ctx_task_touches(lua_State* L) -> int {
  return guarded(L, [L] { return ctx_read(L, "ctx.task_touches", {"task", "touches", "list"}); });
}
extern "C" auto host_ctx_recommend_strategy(lua_State* L) -> int {
  return guarded(L, [L] { return ctx_read(L, "ctx.recommend_strategy", {"plan", "recommend-strategy"}); });
}

/// @brief `ctx.context([stage])` — an empty table.
///
/// Not a stub: this is what the oracle returns. The engine has no run
/// binding (one clean process per phase, no scheduler), so there is no run
/// id to read context for, and fabricating one would be worse than
/// returning nothing. The verb stays registered so the frozen surface
/// matches D7's allowlist and a later milestone can wire the binding behind
/// the same lock.
extern "C" auto host_ctx_context(lua_State* L) -> int {
  return guarded(L, [L] {
    lua_createtable(L, 0, 0);
    return 1;
  });
}

/// @brief `ctx.brief(opts)` — renders the coder brief from the plan, task,
/// authoritative packet, and `planar-agent` schema.
///
/// Port target: `host.zig`'s `hostCtxBrief`, over the `state`/`schema`/
/// `brief` namespaces this task ported (task 6125; plan 996). All twenty-five
/// host functions are now complete.
///
/// `opts` table fields: `plan_id` (int, required), `task_id` (int,
/// required), `claim_token` (string, required), `problem_statement`
/// (string, required), `gates` (array of strings, optional).
///
/// The error-message mapping below matches the oracle's exactly: a
/// `plan`/`task` shell-or-parse failure both collapse to "not found" (the
/// oracle catches a `state.zig` `StateError` union covering both), while the
/// authoritative-packet shell failure surfaces `run_allowlisted`'s own
/// message unchanged (`host.zig` only wraps the JSON-parse half of that
/// call, not the shell).
extern "C" auto host_ctx_brief(lua_State* L) -> int {
  return guarded(L, [L] -> int {
    auto& hs = upvalue_state(L);
    if (lua_type(L, 1) != LUA_TTABLE) {
      fail("ctx.brief expects an opts table");
    }

    auto const plan_id = table_int_field(L, 1, "plan_id");
    if (!plan_id.has_value()) {
      fail("ctx.brief: opts.plan_id required");
    }
    auto const task_id = table_int_field(L, 1, "task_id");
    if (!task_id.has_value()) {
      fail("ctx.brief: opts.task_id required");
    }
    auto const claim_token = table_str_field(L, 1, "claim_token");
    if (!claim_token.has_value()) {
      fail("ctx.brief: opts.claim_token required");
    }
    auto const problem_statement = table_str_field(L, 1, "problem_statement");
    if (!problem_statement.has_value()) {
      fail("ctx.brief: opts.problem_statement required");
    }

    std::vector<std::string> gates;
    lua_getfield(L, 1, "gates");
    if (lua_type(L, -1) == LUA_TTABLE) {
      gates = argv_from_table(L, lua_absindex(L, -1));
    }
    lua_settop(L, -2);

    auto const plan_id_str = std::format("{}", *plan_id);
    auto const task_id_str = std::format("{}", *task_id);

    std::string plan_json;
    try {
      plan_json = run_allowlisted(hs, "planar", {"plan", "show", plan_id_str, "--json"});
    } catch (host_error const&) {
      fail("ctx.brief: plan {} not found", *plan_id);
    }
    auto plan_parsed = state::parse_plan_show(plan_json);
    if (!plan_parsed.has_value()) {
      fail("ctx.brief: plan {} not found", *plan_id);
    }

    std::string task_json;
    try {
      task_json = run_allowlisted(hs, "planar", {"task", "show", task_id_str, "--json"});
    } catch (host_error const&) {
      fail("ctx.brief: task {} not found", *task_id);
    }
    auto task_parsed = state::parse_task_show(task_json);
    if (!task_parsed.has_value()) {
      fail("ctx.brief: task {} not found", *task_id);
    }

    std::string schema_json;
    try {
      schema_json = run_allowlisted(hs, "planar-agent", {"schema"});
    } catch (host_error const&) {
      fail("ctx.brief: failed to load planar-agent schema");
    }
    auto schema_parsed = schema::parse_raw_schema(schema_json);
    if (!schema_parsed.has_value()) {
      fail("ctx.brief: failed to load planar-agent schema");
    }
    schema::bin_schema const agent_schema{std::move(*schema_parsed)};

    // Unlike plan/task above, a shell failure here is NOT remapped — the
    // oracle only wraps the JSON-parse half of this particular call.
    auto const packet_json   = run_allowlisted(hs, "planar", {"task", "packet", task_id_str, "--json"});
    auto       packet_parsed = state::parse_task_packet(packet_json);
    if (!packet_parsed.has_value()) {
      fail("ctx.brief: authoritative packet parse failed");
    }
    if (!packet_parsed->ready()) {
      fail("ctx.brief: authoritative packet not ready");
    }

    // Adapt task_show -> the task_entry shape compile_brief consumes: the
    // title comes from the AUTHORITATIVE packet, not the plain task-show
    // read (matches host.zig's hostCtxBrief exactly).
    state::task_entry adapted;
    adapted.id      = task_parsed->id;
    adapted.plan_id = *plan_id;
    adapted.title   = packet_parsed->input.title;
    adapted.slug    = task_parsed->slug;
    adapted.status  = task_parsed->status;

    brief::brief_inputs inputs;
    inputs.authoritative_packet = std::move(*packet_parsed);
    inputs.plan                 = std::move(*plan_parsed);
    inputs.tasks                = {adapted};
    inputs.claim_token          = *claim_token;
    inputs.problem_statement    = *problem_statement;
    inputs.agent_schema         = agent_schema;
    inputs.gates                = std::move(gates);

    auto compiled = brief::compile_brief(inputs);
    if (!compiled.has_value()) {
      fail("ctx.brief: caller plan/task/claim does not match authoritative packet");
    }
    lua_pushlstring(L, compiled->data(), compiled->size());
    return 1;
  });
}

// ---------------------------------------------------------------------------
// Registrar
// ---------------------------------------------------------------------------

/// @brief Map a manifest entry to its implementation.
///
/// The registrar is DATA-DRIVEN off `allowed_host_fns()` and every function
/// it installs comes from this switch, so a function cannot be registered
/// without a manifest entry naming it. A missing arm is not a silent gap
/// either: `install_host_surface` treats a null dispatch as a fatal
/// registration failure rather than skipping the entry.
/// @param entry The manifest entry.
/// @return The C function, or null when the manifest names something
/// unimplemented.
auto dispatch_for(host_fn const& entry) -> lua_CFunction {
  using pair = std::pair<std::string_view, lua_CFunction>;
  static constexpr std::array<pair, 25> table{{
      {"cli.planar", host_cli_planar},
      {"cli.planar_agent", host_cli_planar_agent},
      {"cli.planar_agent_json", host_cli_planar_agent_json},
      {"cli.planar_json", host_cli_planar_json},
      {"cli.planar_watch", host_cli_planar_watch},
      {"cli.planar_watch_json", host_cli_planar_watch_json},
      {"git.checkout", host_git_checkout},
      {"git.clean", host_git_clean},
      {"git.diff_name_only", host_git_diff_name_only},
      {"git.head_sha", host_git_head_sha},
      {"git.reset_hard", host_git_reset_hard},
      {"fs.exists", host_fs_exists},
      {"fs.mkdir", host_fs_mkdir},
      {"fs.read", host_fs_read},
      {"fs.write", host_fs_write},
      {"flow.fail", host_flow_fail},
      {"flow.log", host_flow_log},
      {"flow.phase", host_flow_phase},
      {"flow.result", host_flow_result},
      {"ctx.brief", host_ctx_brief},
      {"ctx.context", host_ctx_context},
      {"ctx.plan_show", host_ctx_plan_show},
      {"ctx.recommend_strategy", host_ctx_recommend_strategy},
      {"ctx.task_show", host_ctx_task_show},
      {"ctx.task_touches", host_ctx_task_touches},
  }};
  auto const                            key = std::format("{}.{}", entry.table, entry.name);
  auto const                            it  = std::ranges::find(table, key, &pair::first);
  return it == table.end() ? nullptr : it->second;
}

/// @brief Open the curated stdlib and strip every escape hatch.
///
/// See execute.cppm's header for the derived list and how it was derived
/// (by enumerating the oracle's live globals, not by reading its source).
/// @param L The Lua state.
auto open_sandboxed_libs(lua_State* L) -> void {
  struct lib {
    char const*   name;
    lua_CFunction open;
  };
  std::array<lib, 5> const libs{{
      {LUA_GNAME, luaopen_base},
      {LUA_TABLIBNAME, luaopen_table},
      {LUA_STRLIBNAME, luaopen_string},
      {LUA_MATHLIBNAME, luaopen_math},
      {LUA_UTF8LIBNAME, luaopen_utf8},
  }};
  // NOT luaL_openlibs: that would open os, io, package, coroutine and debug
  // and leave five separate holes to plug afterwards. Opening the subset is
  // the difference between a sandbox and a suggestion.
  for (auto const& entry : libs) {
    luaL_requiref(L, entry.name, entry.open, 1);
    lua_settop(L, -2); // pop the module table requiref leaves behind
  }

  if (lua_getglobal(L, "math") == LUA_TTABLE) {
    int const math_idx = lua_absindex(L, -1);
    for (char const* field : {"random", "randomseed"}) {
      lua_pushnil(L);
      lua_setfield(L, math_idx, field);
    }
  }
  lua_settop(L, -2);

  // The loader hatches. `loadstring` is 5.1 vocabulary and 5.5 does not
  // define it; nil'ing it anyway costs nothing and survives a future bump
  // that reinstates a compatibility alias.
  for (char const* global : {"dofile", "loadfile", "load", "loadstring", "require"}) {
    lua_pushnil(L);
    lua_setglobal(L, global);
  }
}

/// @brief Build the five host tables and populate them from the manifest.
/// @param L The Lua state.
/// @param hs The host state, captured as upvalue #1 of every closure.
auto install_host_surface(lua_State* L, host_state& hs) -> void {
  for (auto const table : host_tables()) {
    lua_createtable(L, 0, 8);
    for (auto const& entry : allowed_host_fns()) {
      if (entry.table != table) {
        continue;
      }
      auto* const fn = dispatch_for(entry);
      if (fn == nullptr) {
        // A manifest entry with no implementation. The table under
        // construction is left ALONE here — an earlier spelling popped it and
        // every subsequent `lua_setfield` in the loop then wrote into
        // whatever was beneath, which turned one missing arm into a corrupt
        // state and a cascade of unrelated failures. Skipping cleanly leaves
        // exactly one observable symptom: the function is absent from the
        // live surface, which is precisely what `surface.t.cpp` compares
        // against the manifest. (Verified by break-probe: removing a dispatch
        // arm fails that comparison and nothing else spuriously.)
        continue;
      }
      lua_pushlightuserdata(L, &hs);
      lua_pushcclosure(L, fn, 1);
      std::string const name{entry.name};
      lua_setfield(L, -2, name.c_str());
    }
    std::string const table_name{table};
    lua_setglobal(L, table_name.c_str());
  }

  // Determinism injection. ctx.now / ctx.seed are the ONLY clock and seed a
  // workflow can reach (os and math.random are gone), and both default to 0,
  // so a run is reproducible unless the caller deliberately injects real
  // values.
  lua_getglobal(L, "ctx");
  int const ctx_idx = lua_absindex(L, -1);
  lua_pushinteger(L, static_cast<lua_Integer>(hs.config->now));
  lua_setfield(L, ctx_idx, "now");
  lua_pushinteger(L, static_cast<lua_Integer>(hs.config->seed));
  lua_setfield(L, ctx_idx, "seed");
  auto const args = trim(hs.config->args_json);
  if (args.empty()) {
    lua_createtable(L, 0, 0);
  } else {
    json_reader{args}.parse_into(L);
  }
  lua_setfield(L, ctx_idx, "args");
  lua_settop(L, ctx_idx - 1);
}

/// @brief Format the error value on top of the stack and pop it.
/// @param L The Lua state.
/// @param stage `load`, `init` or `phase`.
/// @param err The diagnostic stream.
auto report_lua_error(lua_State* L, std::string_view stage, std::ostream& err) -> void {
  std::size_t      len = 0;
  char const*      raw = luaL_tolstring(L, -1, &len);
  std::string_view message{"(unknown error)"};
  if (raw != nullptr) {
    message = std::string_view{raw, len};
  }
  err << "planar-execute: " << stage << " error: " << message << '\n';
  lua_settop(L, -3); // pop the coerced string and the original error
}

/// @brief A `lua_State` that closes itself.
class lua_state_handle {
public:
  lua_state_handle() : _state(luaL_newstate()) {
  }
  lua_state_handle(lua_state_handle const&)                    = delete;
  auto operator=(lua_state_handle const&) -> lua_state_handle& = delete;
  ~lua_state_handle() {
    if (_state != nullptr) {
      lua_close(_state);
    }
  }
  /// @brief The state.
  /// @return The pointer, null when creation failed.
  [[nodiscard]] auto get() const -> lua_State* {
    return _state;
  }

private:
  lua_State* _state = nullptr;
};

/// @brief Build a state with the sandbox and the host surface installed,
/// for the introspection entry points.
/// @param handle Receives the state.
/// @param hs Receives the host state (must outlive the handle's use).
/// @param config Receives the config the host state points at.
auto make_introspection_state(lua_state_handle& handle, host_state& hs, run_config& config, std::ostream& sink) -> bool {
  if (handle.get() == nullptr) {
    return false;
  }
  hs.config = &config;
  hs.err    = &sink;
  open_sandboxed_libs(handle.get());
  install_host_surface(handle.get(), hs);
  return true;
}

/// @brief Enumerate the table on top of the stack; pops nothing.
/// @param L The Lua state.
/// @param idx The table's index.
/// @return Its entries, sorted by name.
auto enumerate_table(lua_State* L, int idx) -> std::vector<value_entry> {
  std::vector<value_entry> entries;
  int const                abs = lua_absindex(L, idx);
  lua_pushnil(L);
  while (lua_next(L, abs) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      std::size_t len = 0;
      char const* raw = lua_tolstring(L, -2, &len);
      entries.push_back(value_entry{.name = std::string{raw, len}, .type = lua_typename(L, lua_type(L, -1))});
    }
    lua_settop(L, -2);
  }
  std::ranges::sort(entries, {}, &value_entry::name);
  return entries;
}

} // namespace

// ---------------------------------------------------------------------------
// Exported entry points
// ---------------------------------------------------------------------------

auto run_workflow(run_config const& config, std::ostream& out, std::ostream& err) -> run_status {
  lua_state_handle handle;
  if (handle.get() == nullptr) {
    err << "planar-execute: cannot initialize the Lua sandbox\n";
    return run_status::init_failed;
  }
  lua_State* L = handle.get();

  host_state hs;
  hs.config = &config;
  hs.err    = &err;

  open_sandboxed_libs(L);
  // Installing the surface decodes `--args` into `ctx.args`, and that decode
  // runs HERE, outside any `lua_pcall` and outside `guarded()`, so a
  // `host_error` from it is not converted into a Lua error by anything. Until
  // task 6483 it escaped as an uncaught C++ exception and aborted the process
  // (exit 134) on `--args '{bad'`. The shared JSON reader's wording names the
  // "host" because it was written for sibling-binary output; the caller wrote
  // `--args`, so the refusal says so, as a stage error like load and init.
  try {
    install_host_surface(L, hs);
  } catch (host_error const& e) {
    std::string_view const what{e.what()};
    err << "planar-execute: args error: "
        << (what.contains("nested too deeply") ? "--args is nested too deeply" : "--args is not valid JSON") << '\n';
    return run_status::load_failed;
  }

  // The chunk name is "@workflow" so Lua's own diagnostics read
  // `workflow:1: …` — the oracle's exact prefix, which every load/init/phase
  // error message inherits.
  if (luaL_loadbufferx(L, config.source.data(), config.source.size(), "@workflow", nullptr) != LUA_OK) {
    report_lua_error(L, "load", err);
    return run_status::load_failed;
  }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    report_lua_error(L, "init", err);
    return run_status::load_failed;
  }

  // Phases are plain globals the chunk defined while it ran.
  if (lua_getglobal(L, config.phase.c_str()) != LUA_TFUNCTION) {
    err << "planar-execute: phase function not found: " << config.phase << '\n';
    return run_status::phase_missing;
  }
  if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
    // `flow.fail` raises too, so prefer the message it recorded over the
    // Lua error text — the latter carries a `workflow:N:` prefix and the
    // `flow.fail: ` marker, neither of which the operator asked for.
    if (hs.fail_msg.has_value()) {
      err << "planar-execute: phase failed: " << *hs.fail_msg << '\n';
    } else {
      report_lua_error(L, "phase", err);
    }
    return run_status::phase_failed;
  }

  // Always JSON on stdout: a phase that declared no result still produces a
  // parseable document, so a caller never has to special-case empty output.
  out << (hs.result_json.has_value() ? *hs.result_json : std::string{"{}"}) << '\n';
  return run_status::ok;
}

auto registered_host_surface() -> std::vector<std::pair<std::string, std::string>> {
  lua_state_handle                                 handle;
  host_state                                       hs;
  run_config                                       config;
  std::ostringstream                               sink;
  std::vector<std::pair<std::string, std::string>> found;
  if (!make_introspection_state(handle, hs, config, sink)) {
    return found;
  }
  lua_State* L = handle.get();
  for (auto const table : host_tables()) {
    std::string const name{table};
    if (lua_getglobal(L, name.c_str()) != LUA_TTABLE) {
      lua_settop(L, -2);
      continue;
    }
    for (auto const& entry : enumerate_table(L, -1)) {
      if (entry.type == "function") {
        found.emplace_back(name, entry.name);
      }
    }
    lua_settop(L, -2);
  }
  std::ranges::sort(found);
  return found;
}

auto sandbox_globals() -> std::vector<value_entry> {
  lua_state_handle   handle;
  host_state         hs;
  run_config         config;
  std::ostringstream sink;
  if (!make_introspection_state(handle, hs, config, sink)) {
    return {};
  }
  lua_State* L = handle.get();
  lua_pushglobaltable(L);
  auto entries = enumerate_table(L, -1);
  lua_settop(L, -2);
  return entries;
}

auto sandbox_table_entries(std::string_view table) -> std::vector<value_entry> {
  lua_state_handle   handle;
  host_state         hs;
  run_config         config;
  std::ostringstream sink;
  if (!make_introspection_state(handle, hs, config, sink)) {
    return {};
  }
  lua_State*        L = handle.get();
  std::string const name{table};
  if (lua_getglobal(L, name.c_str()) != LUA_TTABLE) {
    lua_settop(L, -2);
    return {};
  }
  auto entries = enumerate_table(L, -1);
  lua_settop(L, -2);
  return entries;
}

auto lua_version() -> std::string {
  lua_state_handle handle;
  if (handle.get() == nullptr) {
    return {};
  }
  lua_State* L = handle.get();
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  lua_settop(L, -2);
  if (lua_getglobal(L, "_VERSION") != LUA_TSTRING) {
    lua_settop(L, -2);
    return {};
  }
  std::size_t len = 0;
  char const* raw = lua_tolstring(L, -1, &len);
  std::string version{raw, len};
  lua_settop(L, -2);
  return version;
}

} // namespace planar::engine::execute

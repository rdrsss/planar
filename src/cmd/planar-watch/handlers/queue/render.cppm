/// @file render.cppm
/// @brief `planar.cmd.planar_watch.handlers.queue.render` — the escaping,
/// quoting, duration and table helpers the two host-queue views
/// (`planar-watch queue` and `planar-watch queue history`) share (plan 1080,
/// tasks hq-watch-queue and hq-watch-history).
///
/// A submitted value (argv word, label, vendor, role, directory) may hold any
/// bytes. In text a value containing a control character, DEL, a C1 control, a
/// Unicode format character (bidirectional override, zero-width, line or
/// paragraph separator, byte-order mark), invalid UTF-8, a space, a double
/// quote or a backslash is written as a double-quoted string with `\n`,
/// `\u00xx`, `\u202e` and similar escapes (`planar.textview`), so no value can
/// start a line of its own, move the cursor or reorder the text around it. A
/// shell-quoted argv word holding a control or format character is shown the
/// same way, double-quoted and escaped: that form is display text and is NOT a
/// pasteable shell word. In text a vendor, role or label whose escaped form would pass
/// `textview::k_field_cap` columns is cut and marked with `…`
/// (`field_cell`), and columns are padded by display width, not bytes. JSON is
/// never cut and never uses the text escapes: `quote` there escapes the
/// grammar's own characters plus DEL and C1 controls, and nothing else.
///
/// Pure string functions; nothing here fails, opens a store or writes.
module;

export module planar.cmd.planar_watch.handlers.queue.render;

import std;

namespace planar::cmd::watch::handlers::queue_render {

/// @brief `value` as a double-quoted string, escaping the quote, the
/// backslash and every control byte (C0, DEL, C1). The result is a valid JSON
/// string.
/// @param value The bytes to quote.
/// @return The quoted text.
export auto quote(std::string_view value) -> std::string;

/// @brief A value as one whitespace-delimited cell: `-` when empty, quoted
/// when it holds a control byte, format character, space, double quote or
/// backslash, or is itself `-`.
/// @param value The bytes to show.
/// @return The cell text.
export auto cell(std::string_view value) -> std::string;

/// @brief An argument vector as one line, each word shell-quoted (bare when
/// only safe characters, single-quoted otherwise, double-quoted with escapes
/// when it holds a control or format character, which is not a pasteable shell
/// word); `-` for an empty vector.
/// @param argv The command and its arguments.
/// @return The line.
export auto shell_line(const std::vector<std::string>& argv) -> std::string;

/// @brief A vendor, role or label as a text-view cell: cut to
/// `textview::k_field_cap` display columns with a `…` marker when longer, then
/// shown as `cell` does.
/// @param value The value as submitted.
/// @return The cell text.
export auto field_cell(std::string_view value) -> std::string;

/// @brief A duration as `-` (absent), `<s>s`, `<m>m<ss>s` or `<h>h<mm>m`.
/// @param ms The milliseconds, when known.
/// @return The text.
export auto duration_text(const std::optional<std::int64_t>& ms) -> std::string;

/// @brief A duration between two wall-clock readings, never negative.
/// @param from The earlier reading, ms.
/// @param to The later reading, ms.
/// @return `to - from`, clamped at zero.
export auto span_ms(std::int64_t from, std::int64_t to) -> std::int64_t;

/// @brief Lays rows out in columns padded to the widest cell by display width
/// (`textview::display_width`), two spaces apart; the last column is not padded. Every row must have the same number
/// of cells as the first.
/// @param table The rows, header first.
/// @return The text, one line per row.
export auto pad_table(const std::vector<std::vector<std::string>>& table) -> std::string;

/// @brief Writes `,"key":` (or `"key":` right after an opening brace) onto a
/// JSON object under construction.
/// @param out The JSON text so far.
/// @param key The member name.
export void put_key(std::string& out, std::string_view key);

/// @brief Writes an integer member, `null` when absent.
/// @param out The JSON text so far.
/// @param key The member name.
/// @param value The value, when any.
export void put_int(std::string& out, std::string_view key, const std::optional<std::int64_t>& value);

/// @brief Writes a string member, `null` when absent.
/// @param out The JSON text so far.
/// @param key The member name.
/// @param value The value, when any.
export void put_text(std::string& out, std::string_view key, const std::optional<std::string>& value);

/// @brief Writes an `argv` array member.
/// @param out The JSON text so far.
/// @param argv The command and its arguments.
export void put_argv(std::string& out, const std::vector<std::string>& argv);

} // namespace planar::cmd::watch::handlers::queue_render

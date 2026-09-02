/// Implementation intentionally uses the grammar AST, never a hand parser.
module;
#include <tree_sitter/api.h>
extern "C" const TSLanguage* tree_sitter_zig(void);
module                       planar.engine.closure.compute;
import std;
import planar.db;
namespace planar::engine::closure::compute {
namespace {
struct definition {
  std::int64_t repo{};
  std::string  path, source;
  uint32_t     first{}, last{};
};
auto stem(std::filesystem::path const& p) -> std::string {
  return p.stem().string();
}
auto child_identifier(TSNode n, std::string_view source) -> std::optional<std::string_view> {
  for (uint32_t i = 0; i < ts_node_child_count(n); ++i) {
    auto c = ts_node_child(n, i);
    if (ts_node_is_named(c) && std::string_view{ts_node_type(c)} == "identifier")
      return source.substr(ts_node_start_byte(c), ts_node_end_byte(c) - ts_node_start_byte(c));
  }
  return std::nullopt;
}
auto test_name(TSNode n, std::string_view source) -> std::string_view {
  for (uint32_t i = 0; i < ts_node_child_count(n); ++i) {
    auto c = ts_node_child(n, i);
    if (std::string_view{ts_node_type(c)} == "string")
      for (uint32_t j = 0; j < ts_node_child_count(c); ++j) {
        auto x = ts_node_child(c, j);
        if (std::string_view{ts_node_type(x)} == "string_content")
          return source.substr(ts_node_start_byte(x), ts_node_end_byte(x) - ts_node_start_byte(x));
      }
  }
  return {};
}
// Tree-sitter omits comments and error tokens. Zig's tokenizer counts both as
// context-window positions, so weights must be lexical rather than AST leaves.
// A bounded port of `std.zig.Tokenizer`: comments are trivia except docs,
// and punctuation is consumed greedily using the tokenizer's complete
// compound-operator vocabulary.  This is deliberately lexical rather than a
// tree-sitter leaf count because the grammar drops ordinary comments/errors.
auto tokens(std::string_view s) -> std::int64_t {
  std::int64_t count = 0;
  for (std::size_t i = 0; i < s.size();) {
    if (std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
      continue;
    }
    if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      auto const doc = i + 2 < s.size() && (s[i + 2] == '/' || s[i + 2] == '!');
      i              = s.find('\n', i + 2);
      if (doc)
        ++count;
      if (i == std::string_view::npos)
        break;
      continue;
    }
    ++count;
    if (static_cast<unsigned char>(s[i]) < 0x20 && s[i] != '\n' && s[i] != '\r' && s[i] != '\t') {
      // Tokenizer's `.invalid` state consumes the remainder of its physical
      // line before yielding a single invalid token.
      i = s.find('\n', i + 1);
      if (i == std::string_view::npos)
        break;
      continue;
    }
    if (s[i] == '"' || s[i] == '\'') {
      const auto quote = s[i++];
      while (i < s.size() && s[i] != quote) {
        if (s[i++] == '\\' && i < s.size())
          ++i;
      }
      if (i < s.size())
        ++i;
      continue;
    }
    if (s[i] == '@' && i + 1 < s.size() && (std::isalpha(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '_')) {
      // `@import`/`@TypeOf` are one `.builtin` token, not punctuation plus
      // identifier.  Keywords and identifiers both remain one token.
      i += 2;
      while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
        ++i;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(s[i]))) {
      // Number literals may contain a base prefix, separators, exponent and
      // decimal point.  Stop before a range operator so `1..2` remains three
      // tags, as it does in std.zig.Tokenizer.
      ++i;
      while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_' ||
                              (s[i] == '.' && !(i + 1 < s.size() && s[i + 1] == '.'))))
        ++i;
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_') {
      while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_'))
        ++i;
      continue;
    }
    // Longest-match spellings from std.zig.Tokenizer.Tag.  Counting one tag
    // per spelling is the only quantity closure weights retain.
    // Kept against every multi-byte punctuation tag in std.zig.Tokenizer.Tag:
    // |= || == => != %= ^= ++ += +% +%= +| +|= -= -% -%= -| -|= *= **
    // *% *%= *| *|= -> /= &= <= << <<= <<| <<|= >= >> >>= .. ... .* .**.
    static constexpr std::array<std::string_view, 40> compounds{
        "<<|=", "<<|", "<<=", ">>=", "**%=", "*%=", "+%=", "-%=", "+|=", "-|=", "*|=", "...", ".**", "==",
        "=>",   "!=",  "||",  "|=",  "%=",   "^=",  "+=",  "++",  "+%",  "+|",  "-=",  "-%",  "-|",  "*=",
        "**",   "*%",  "*|",  "->",  "/=",   "&=",  "<=",  "<<",  ">=",  ">>",  "..",  ".*"};
    bool matched = false;
    for (auto op : compounds)
      if (s.substr(i).starts_with(op)) {
        i += op.size();
        matched = true;
        break;
      }
    if (matched)
      continue;
    ++i;
  }
  return count;
}
struct reference {
  std::string raw;
  uint32_t    offset{};
};
auto references(TSNode node, std::string_view source, std::vector<reference>& names, TSNode parent = {}) -> void {
  auto const kind             = std::string_view{ts_node_type(node)};
  auto const parent_kind      = ts_node_is_null(parent) ? std::string_view{} : std::string_view{ts_node_type(parent)};
  auto       declaration_name = false;
  if (parent_kind == "function_declaration" || parent_kind == "variable_declaration")
    for (uint32_t i = 0; i < ts_node_child_count(parent); ++i) {
      auto child = ts_node_child(parent, i);
      if (std::string_view{ts_node_type(child)} == "identifier") {
        declaration_name =
            ts_node_start_byte(node) == ts_node_start_byte(child) && ts_node_end_byte(node) == ts_node_end_byte(child);
        break;
      }
    }
  if (kind == "identifier" && parent_kind != "field_expression" && !declaration_name)
    names.push_back({std::string{source.substr(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node))},
                     ts_node_start_byte(node)});
  if (kind == "field_expression") {
    std::vector<TSNode> ids;
    for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
      auto c = ts_node_child(node, i);
      if (std::string_view{ts_node_type(c)} == "identifier")
        ids.push_back(c);
    }
    if (ids.size() >= 2)
      names.push_back(
          {std::format(
               "{}.{}",
               source.substr(ts_node_start_byte(ids.front()), ts_node_end_byte(ids.front()) - ts_node_start_byte(ids.front())),
               source.substr(ts_node_start_byte(ids.back()), ts_node_end_byte(ids.back()) - ts_node_start_byte(ids.back()))),
           ts_node_start_byte(node)});
  }
  for (uint32_t i = 0; i < ts_node_child_count(node); ++i)
    references(ts_node_child(node, i), source, names, node);
}
auto self_references(TSNode node, std::string_view source, std::string_view container, std::vector<reference>& names) -> void {
  if (std::string_view{ts_node_type(node)} == "field_expression") {
    std::vector<TSNode> ids;
    for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
      auto c = ts_node_child(node, i);
      if (std::string_view{ts_node_type(c)} == "identifier")
        ids.push_back(c);
    }
    if (ids.size() >= 2 &&
        source.substr(ts_node_start_byte(ids.front()), ts_node_end_byte(ids.front()) - ts_node_start_byte(ids.front())) == "self")
      names.push_back({std::format("{}.{}", container,
                                   source.substr(ts_node_start_byte(ids.back()),
                                                 ts_node_end_byte(ids.back()) - ts_node_start_byte(ids.back()))),
                       ts_node_start_byte(node)});
  }
  for (uint32_t i = 0; i < ts_node_child_count(node); ++i)
    self_references(ts_node_child(node, i), source, container, names);
}
auto qualified_key(std::int64_t repo, std::string_view symbol) -> std::string {
  return std::format("{}\x1f{}", repo, symbol);
}
auto imported_containers(std::string_view source) -> std::map<std::string, std::string, std::less<>> {
  // `const Foo = @import("foo.zig").Foo;` lets callers spell a typed
  // receiver as `Foo`, rather than `foo.Foo`.  Keep that alias local to the
  // seed source: imports do not create global names.
  std::map<std::string, std::string, std::less<>> result;
  static const std::regex                         import{
      R"import(const\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*@import\("([^"]+)"\)\.([A-Za-z_][A-Za-z0-9_]*))import"};
  std::string text{source};
  for (std::sregex_iterator it(text.begin(), text.end(), import), end; it != end; ++it)
    result.emplace((*it)[1].str(), std::format("{}.{}", stem((*it)[2].str()), (*it)[3].str()));
  return result;
}
auto receiver_container(std::string_view source, std::string_view receiver,
                        std::map<std::string, std::string, std::less<>> const& imports) -> std::optional<std::string> {
  // This deliberately accepts pointer, optional and generic decorations. The
  // AST tells us `receiver.member`; the declaration provides its container.
  const std::regex typed{std::format(
      R"((?:var|const)\s+{}\s*:\s*(?:(?:\?\s*)|(?:\*+\s*(?:(?:const|volatile)\s+)?)|(?:\[\]\s*(?:const\s+)?))*([A-Za-z_][A-Za-z0-9_]*)(?:\.([A-Za-z_][A-Za-z0-9_]*))?(?:\s*\([^;=)]*\))?)",
      receiver)};
  std::string      text{source};
  std::smatch      match;
  if (!std::regex_search(text, match, typed))
    return std::nullopt;
  if (match[2].matched)
    return std::format("{}.{}", match[1].str(), match[2].str());
  if (auto it = imports.find(match[1].str()); it != imports.end())
    return it->second;
  return std::nullopt;
}
struct receiver_binding {
  std::string name, container;
  uint32_t    start{}, scope_end{};
};
auto receiver_bindings(TSNode node, std::string_view source, std::map<std::string, std::string, std::less<>> const& imports,
                       std::vector<receiver_binding>& result, uint32_t scope_end) -> void {
  auto const kind = std::string_view{ts_node_type(node)};
  if (kind == "variable_declaration") {
    if (auto name = child_identifier(node, source))
      if (auto container = receiver_container(
              source.substr(ts_node_start_byte(node), ts_node_end_byte(node) - ts_node_start_byte(node)), *name, imports))
        result.push_back({std::string{*name}, std::move(*container), ts_node_start_byte(node), scope_end});
  }
  for (uint32_t i = 0; i < ts_node_child_count(node); ++i) {
    auto child           = ts_node_child(node, i);
    auto child_scope_end = scope_end;
    if (std::string_view{ts_node_type(child)} == "block")
      child_scope_end = ts_node_end_byte(child);
    receiver_bindings(child, source, imports, result, child_scope_end);
  }
}
auto receiver_container(std::vector<receiver_binding> const& bindings, std::string_view receiver, uint32_t offset)
    -> std::optional<std::string> {
  std::optional<std::string> result;
  uint32_t                   latest = 0;
  for (auto const& binding : bindings)
    if (binding.name == receiver && binding.start < offset && offset < binding.scope_end && binding.start >= latest) {
      result = binding.container;
      latest = binding.start;
    }
  return result;
}
} // namespace
auto run(db::connection& conn, std::int64_t task_id) -> std::expected<result, error> {
  auto seeds = conn.prepare("select t.repo_id,t.path,p.root_path from task_touch_paths t join projects p on p.id=t.repo_id where "
                            "t.task_id=? order by t.repo_id,t.path");
  if (!seeds)
    return std::unexpected(error::query_failed);
  if (!seeds->bind_int64(1, task_id))
    return std::unexpected(error::query_failed);
  struct seed {
    std::int64_t repo;
    std::string  path, root;
  };
  std::vector<seed> all;
  while (true) {
    auto s = seeds->step();
    if (!s)
      return std::unexpected(error::query_failed);
    if (*s == db::step_result::done)
      break;
    all.push_back({seeds->column_int64(0), seeds->column_text(1), seeds->is_null(2) ? std::string{} : seeds->column_text(2)});
  }
  if (all.empty())
    return std::unexpected(error::no_seeds);
  struct row {
    std::int64_t repo;
    std::string  path, symbol, role;
    std::int64_t weight;
  };
  std::vector<row> rows;
  // Build a recursive, hidden-directory-filtered corpus before resolving any
  // seed.  Definitions carry source spans so reference and transitive rows
  // receive the defining declaration's weight, not the caller's.
  std::map<std::string, definition, std::less<>> defs;
  std::map<std::filesystem::path, std::int64_t>  roots;
  for (auto const& s : all)
    if (!s.root.empty())
      roots.emplace(s.root, s.repo);
  for (auto const& [root_path, repo] : roots) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(root_path, ec), end; !ec && it != end; it.increment(ec)) {
      auto const p = it->path();
      if (it->is_directory() && p.filename().string().starts_with(".")) {
        it.disable_recursion_pending();
        continue;
      }
      if (p.filename().string().starts_with("."))
        continue;
      if (p.extension() != ".zig")
        continue;
      std::ifstream f(p, std::ios::binary);
      std::string   text((std::istreambuf_iterator<char>(f)), {});
      TSParser*     parser = ts_parser_new();
      ts_parser_set_language(parser, tree_sitter_zig());
      TSTree* tree      = ts_parser_parse_string(parser, nullptr, text.data(), static_cast<uint32_t>(text.size()));
      auto    root      = ts_tree_root_node(tree);
      auto    file_stem = stem(p);
      for (uint32_t i = 0; i < ts_node_child_count(root); ++i) {
        auto n = ts_node_child(root, i);
        auto k = std::string_view{ts_node_type(n)};
        if (k != "function_declaration" && k != "variable_declaration" && k != "test_declaration")
          continue;
        auto name = k == "test_declaration" ? std::optional<std::string_view>{test_name(n, text)} : child_identifier(n, text);
        if (!name)
          continue;
        auto rel = std::filesystem::relative(p, root_path, ec).generic_string();
        auto d   = definition{repo, rel, text, ts_node_start_byte(n), ts_node_end_byte(n)};
        defs.emplace(qualified_key(repo, std::format("{}.{}", file_stem, *name)), d);
        if (k == "variable_declaration")
          for (uint32_t j = 0; j < ts_node_child_count(n); ++j) {
            auto c  = ts_node_child(n, j);
            auto ck = std::string_view{ts_node_type(c)};
            if (ck != "struct_declaration" && ck != "enum_declaration" && ck != "union_declaration" && ck != "opaque_declaration")
              continue;
            for (uint32_t m = 0; m < ts_node_child_count(c); ++m) {
              auto member = ts_node_child(c, m);
              auto mn     = child_identifier(member, text);
              if (mn)
                defs.emplace(qualified_key(repo, std::format("{}.{}.{}", file_stem, *name, *mn)), d);
            }
          }
      }
      ts_tree_delete(tree);
      ts_parser_delete(parser);
    }
  }
  for (auto const& s : all) {
    std::ifstream in(std::filesystem::path{s.root} / s.path, std::ios::binary);
    if (!in)
      continue;
    std::string source((std::istreambuf_iterator<char>(in)), {});
    TSParser*   p = ts_parser_new();
    if (!ts_parser_set_language(p, tree_sitter_zig())) {
      ts_parser_delete(p);
      return std::unexpected(error::query_failed);
    }
    TSTree*               t    = ts_parser_parse_string(p, nullptr, source.data(), static_cast<uint32_t>(source.size()));
    auto                  root = ts_tree_root_node(t);
    auto                  st   = stem(s.path);
    std::set<std::string> modifies;
    for (uint32_t i = 0; i < ts_node_child_count(root); ++i) {
      auto n    = ts_node_child(root, i);
      auto type = std::string_view{ts_node_type(n)};
      if (type != "function_declaration" && type != "variable_declaration" && type != "test_declaration")
        continue;
      auto name =
          type == "test_declaration" ? std::optional<std::string_view>{test_name(n, source)} : child_identifier(n, source);
      if (!name)
        continue;
      auto a = ts_node_start_byte(n), b = ts_node_end_byte(n);
      auto symbol = std::format("{}.{}", st, *name);
      modifies.emplace(symbol);
      rows.push_back({s.repo, s.path, std::move(symbol), "modify", tokens(std::string_view{source}.substr(a, b - a))});
    }
    auto                   imports  = imported_containers(source);
    auto                   find_def = [&defs, &s](std::string_view symbol) { return defs.find(qualified_key(s.repo, symbol)); };
    std::set<std::string>  seen_ref;
    std::vector<reference> direct;
    references(root, source, direct);
    // Resolve `self.member` in the lexical container where it occurs. The
    // generic identifier walk intentionally has no parent context, so this
    // focused pass prevents two containers with the same member name from
    // collapsing onto the first corpus definition.
    for (uint32_t i = 0; i < ts_node_child_count(root); ++i) {
      auto n = ts_node_child(root, i);
      if (std::string_view{ts_node_type(n)} != "variable_declaration")
        continue;
      auto name = child_identifier(n, source);
      if (!name)
        continue;
      for (uint32_t j = 0; j < ts_node_child_count(n); ++j) {
        auto c = ts_node_child(n, j);
        auto k = std::string_view{ts_node_type(c)};
        if (k == "struct_declaration" || k == "enum_declaration" || k == "union_declaration" || k == "opaque_declaration")
          self_references(c, source, std::format("{}.{}", st, *name), direct);
      }
    }
    std::vector<receiver_binding> bindings;
    receiver_bindings(root, source, imports, bindings, static_cast<uint32_t>(source.size()));
    for (auto const& ref : direct) {
      auto const& raw = ref.raw;
      auto        q   = raw.contains('.') ? raw : std::format("{}.{}", st, raw);
      auto        d   = find_def(q);
      if (d == defs.end() && raw.contains('.')) {
        auto dot  = raw.find('.');
        auto recv = raw.substr(0, dot), member = raw.substr(dot + 1);
        if (recv != "self") {
          if (auto container = receiver_container(bindings, recv, ref.offset)) {
            q = std::format("{}.{}", *container, member);
            d = find_def(q);
          }
        }
      }
      if (d == defs.end())
        continue;
      // The declaration walk reaches a function's defining identifier too.
      // A modify symbol is not a reference to itself.
      if (modifies.contains(q))
        continue;
      if (seen_ref.emplace(q).second)
        rows.push_back({d->second.repo, d->second.path, q, "reference",
                        tokens(std::string_view{d->second.source}.substr(d->second.first, d->second.last - d->second.first))});
    }
    for (auto const& q : seen_ref) {
      auto d = find_def(q);
      if (d == defs.end())
        continue;
      std::vector<reference> hop;
      TSParser*              hp = ts_parser_new();
      ts_parser_set_language(hp, tree_sitter_zig());
      TSTree* ht =
          ts_parser_parse_string(hp, nullptr, d->second.source.data() + d->second.first, d->second.last - d->second.first);
      references(ts_tree_root_node(ht),
                 std::string_view{d->second.source}.substr(d->second.first, d->second.last - d->second.first), hop);
      auto owner = q.substr(0, q.find('.'));
      for (auto const& ref : hop) {
        auto const& raw = ref.raw;
        auto        hq  = raw.contains('.') ? raw : std::format("{}.{}", owner, raw);
        if (raw.starts_with("self."))
          hq = std::format("{}.{}", q.substr(0, q.rfind('.')), raw.substr(std::string_view{"self."}.size()));
        auto hd = find_def(hq);
        if (hd != defs.end() && !seen_ref.contains(hq))
          rows.push_back(
              {hd->second.repo, hd->second.path, hq, "transitive",
               tokens(std::string_view{hd->second.source}.substr(hd->second.first, hd->second.last - hd->second.first))});
      }
      ts_tree_delete(ht);
      ts_parser_delete(hp);
    }
    ts_tree_delete(t);
    ts_parser_delete(p);
  }
  std::map<std::string, row, std::less<>> pending;
  for (auto const& r : rows)
    pending.emplace(std::format("{}\x1f{}\x1f{}\x1f{}", r.repo, r.path, r.symbol, r.role), r);
  std::size_t modify = 0, reference = 0, transitive = 0;
  for (auto const& [_, r] : pending) {
    if (r.role == "modify")
      ++modify;
    else if (r.role == "reference")
      ++reference;
    else
      ++transitive;
  }
  auto tx = conn.begin_transaction(db::lock_mode::immediate);
  if (!tx)
    return std::unexpected(error::query_failed);
  auto del = conn.prepare("delete from closures where task_id=? and extractor_version=?");
  if (!del || !del->bind_int64(1, task_id) || !del->bind_text(2, extractor_version) || !del->step())
    return std::unexpected(error::query_failed);
  auto ins =
      conn.prepare("insert into closures(task_id,repo_id,path,symbol,role,token_weight,extractor_version) values(?,?,?,?,?,?,?)");
  if (!ins)
    return std::unexpected(error::query_failed);
  for (auto const& [_, r] : pending) {
    if (!ins->bind_int64(1, task_id) || !ins->bind_int64(2, r.repo) || !ins->bind_text(3, r.path) ||
        !ins->bind_text(4, r.symbol) || !ins->bind_text(5, r.role) || !ins->bind_int64(6, r.weight) ||
        !ins->bind_text(7, extractor_version) || !ins->step())
      return std::unexpected(error::query_failed);
    if (!ins->reset())
      return std::unexpected(error::query_failed);
  }
  if (!tx->commit())
    return std::unexpected(error::query_failed);
  return result{task_id, all.size(), modify, reference, transitive, pending.size()};
}
} // namespace planar::engine::closure::compute

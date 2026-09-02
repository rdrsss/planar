/// Implementation intentionally uses the grammar AST, never a hand parser.
module;
#include <tree_sitter/api.h>
extern "C" const TSLanguage* tree_sitter_zig(void);
module planar.engine.closure.compute;
import std;
import planar.db;
namespace planar::engine::closure::compute {
namespace {
struct definition { std::int64_t repo{}; std::string path, source; uint32_t first{}, last{}; };
auto stem(std::filesystem::path const& p) -> std::string { return p.stem().string(); }
auto child_identifier(TSNode n, std::string_view source) -> std::optional<std::string_view> {
  for (uint32_t i=0;i<ts_node_child_count(n);++i) { auto c=ts_node_child(n,i); if (ts_node_is_named(c) && std::string_view{ts_node_type(c)}=="identifier") return source.substr(ts_node_start_byte(c),ts_node_end_byte(c)-ts_node_start_byte(c)); }
  return std::nullopt;
}
auto test_name(TSNode n, std::string_view source) -> std::string_view {
  for(uint32_t i=0;i<ts_node_child_count(n);++i) { auto c=ts_node_child(n,i); if(std::string_view{ts_node_type(c)}=="string") for(uint32_t j=0;j<ts_node_child_count(c);++j){auto x=ts_node_child(c,j);if(std::string_view{ts_node_type(x)}=="string_content")return source.substr(ts_node_start_byte(x),ts_node_end_byte(x)-ts_node_start_byte(x));}}
  return {};
}
auto tokens(std::string_view s) -> std::int64_t { // Tree-sitter token leaves, including punctuation.
  TSParser* p=ts_parser_new(); ts_parser_set_language(p,tree_sitter_zig()); TSTree* t=ts_parser_parse_string(p,nullptr,s.data(),static_cast<uint32_t>(s.size()));
  std::int64_t count=0; std::function<void(TSNode)> walk=[&](TSNode n){ auto k=ts_node_child_count(n); if(k==0 && ts_node_end_byte(n)>ts_node_start_byte(n)) ++count; for(uint32_t i=0;i<k;++i)walk(ts_node_child(n,i));}; walk(ts_tree_root_node(t)); ts_tree_delete(t);ts_parser_delete(p);return count;
}
auto references(TSNode node, std::string_view source, std::set<std::string>& names) -> void {
  auto const kind = std::string_view{ts_node_type(node)};
  if (kind == "identifier") names.emplace(source.substr(ts_node_start_byte(node), ts_node_end_byte(node)-ts_node_start_byte(node)));
  if (kind == "field_expression") {
    std::vector<TSNode> ids; for(uint32_t i=0;i<ts_node_child_count(node);++i){auto c=ts_node_child(node,i);if(std::string_view{ts_node_type(c)}=="identifier")ids.push_back(c);} if(ids.size()>=2) names.emplace(std::format("{}.{}",source.substr(ts_node_start_byte(ids.front()),ts_node_end_byte(ids.front())-ts_node_start_byte(ids.front())),source.substr(ts_node_start_byte(ids.back()),ts_node_end_byte(ids.back())-ts_node_start_byte(ids.back()))));
  }
  for(uint32_t i=0;i<ts_node_child_count(node);++i) references(ts_node_child(node,i),source,names);
}
}
auto run(db::connection& conn, std::int64_t task_id) -> std::expected<result,error> {
  auto seeds=conn.prepare("select t.repo_id,t.path,p.root_path from task_touch_paths t join projects p on p.id=t.repo_id where t.task_id=? order by t.repo_id,t.path"); if(!seeds)return std::unexpected(error::query_failed); if(!seeds->bind_int64(1,task_id))return std::unexpected(error::query_failed);
  struct seed{std::int64_t repo;std::string path,root;}; std::vector<seed> all; while(true){auto s=seeds->step();if(!s)return std::unexpected(error::query_failed);if(*s==db::step_result::done)break;all.push_back({seeds->column_int64(0),seeds->column_text(1),seeds->is_null(2)?std::string{}:seeds->column_text(2)});} if(all.empty())return std::unexpected(error::no_seeds);
  struct row{std::int64_t repo;std::string path,symbol,role;std::int64_t weight;}; std::vector<row> rows;
  // Build a recursive, hidden-directory-filtered corpus before resolving any
  // seed.  Definitions carry source spans so reference and transitive rows
  // receive the defining declaration's weight, not the caller's.
  std::map<std::string, definition, std::less<>> defs;
  std::set<std::filesystem::path> roots; for(auto const& s:all) if(!s.root.empty()) roots.emplace(s.root);
  for(auto const& root_path:roots) { std::error_code ec; for(std::filesystem::recursive_directory_iterator it(root_path,ec), end; !ec&&it!=end; it.increment(ec)) { auto const p=it->path(); if(it->is_directory() && p.filename().string().starts_with(".")){it.disable_recursion_pending();continue;} if(p.extension()!=".zig")continue; std::ifstream f(p,std::ios::binary);std::string text((std::istreambuf_iterator<char>(f)),{}); TSParser* parser=ts_parser_new();ts_parser_set_language(parser,tree_sitter_zig());TSTree* tree=ts_parser_parse_string(parser,nullptr,text.data(),static_cast<uint32_t>(text.size()));auto root=ts_tree_root_node(tree);auto file_stem=stem(p); for(uint32_t i=0;i<ts_node_child_count(root);++i){auto n=ts_node_child(root,i);auto k=std::string_view{ts_node_type(n)};if(k!="function_declaration"&&k!="variable_declaration"&&k!="test_declaration")continue;auto name=k=="test_declaration"?std::optional<std::string_view>{test_name(n,text)}:child_identifier(n,text);if(!name)continue;auto rel=std::filesystem::relative(p,root_path,ec).generic_string();auto d=definition{0,rel,text,ts_node_start_byte(n),ts_node_end_byte(n)};defs.emplace(std::format("{}.{}",file_stem,*name),d);if(k=="variable_declaration")for(uint32_t j=0;j<ts_node_child_count(n);++j){auto c=ts_node_child(n,j);auto ck=std::string_view{ts_node_type(c)};if(ck!="struct_declaration"&&ck!="enum_declaration"&&ck!="union_declaration"&&ck!="opaque_declaration")continue;for(uint32_t m=0;m<ts_node_child_count(c);++m){auto member=ts_node_child(c,m);auto mn=child_identifier(member,text);if(mn)defs.emplace(std::format("{}.{}.{}",file_stem,*name,*mn),d);}}} ts_tree_delete(tree);ts_parser_delete(parser); }}
  for(auto const& s:all){std::ifstream in(std::filesystem::path{s.root}/s.path,std::ios::binary); if(!in)continue;std::string source((std::istreambuf_iterator<char>(in)),{}); TSParser* p=ts_parser_new();if(!ts_parser_set_language(p,tree_sitter_zig())){ts_parser_delete(p);return std::unexpected(error::query_failed);}TSTree* t=ts_parser_parse_string(p,nullptr,source.data(),static_cast<uint32_t>(source.size()));auto root=ts_tree_root_node(t);auto st=stem(s.path);for(uint32_t i=0;i<ts_node_child_count(root);++i){auto n=ts_node_child(root,i);auto type=std::string_view{ts_node_type(n)};if(type!="function_declaration"&&type!="variable_declaration"&&type!="test_declaration")continue;auto name=type=="test_declaration"?std::optional<std::string_view>{test_name(n,source)}:child_identifier(n,source);if(!name)continue;auto a=ts_node_start_byte(n),b=ts_node_end_byte(n);rows.push_back({s.repo,s.path,std::format("{}.{}",st,*name),"modify",tokens(std::string_view{source}.substr(a,b-a))});}
    std::set<std::string> seen_ref, direct; references(root,source,direct); for(auto const& raw:direct){auto q=raw.contains('.')?raw:std::format("{}.{}",st,raw);auto d=defs.find(q);if(d==defs.end()&&raw.contains('.')){auto dot=raw.find('.');auto recv=raw.substr(0,dot),member=raw.substr(dot+1);if(recv=="self"){for(auto const&[candidate,value]:defs)if(candidate.ends_with(std::format(".{}",member))){q=candidate;d=defs.find(q);break;}}else {std::regex typed{std::format("(?:var|const)\\s+{}\\s*:\\s*([A-Za-z_][A-Za-z0-9_]*)\\.([A-Za-z_][A-Za-z0-9_]*)",recv)};std::smatch match;if(std::regex_search(source,match,typed)){q=std::format("{}.{}.{}",match[1].str(),match[2].str(),member);d=defs.find(q);}}}if(d==defs.end())continue;if(seen_ref.emplace(q).second)rows.push_back({s.repo,d->second.path,q,"reference",tokens(std::string_view{d->second.source}.substr(d->second.first,d->second.last-d->second.first))});}
    for(auto const& q:seen_ref){auto d=defs.find(q);if(d==defs.end())continue;std::set<std::string> hop;TSParser* hp=ts_parser_new();ts_parser_set_language(hp,tree_sitter_zig());TSTree* ht=ts_parser_parse_string(hp,nullptr,d->second.source.data()+d->second.first,d->second.last-d->second.first);references(ts_tree_root_node(ht),std::string_view{d->second.source}.substr(d->second.first,d->second.last-d->second.first),hop);auto owner=q.substr(0,q.find('.'));for(auto const& raw:hop){auto hq=raw.contains('.')?raw:std::format("{}.{}",owner,raw);auto hd=defs.find(hq);if(hd!=defs.end()&&!seen_ref.contains(hq))rows.push_back({s.repo,hd->second.path,hq,"transitive",tokens(std::string_view{hd->second.source}.substr(hd->second.first,hd->second.last-hd->second.first))});}ts_tree_delete(ht);ts_parser_delete(hp);}
    ts_tree_delete(t);ts_parser_delete(p);}
  std::map<std::string,row,std::less<>> pending; for(auto const&r:rows) pending.emplace(std::format("{}\x1f{}\x1f{}\x1f{}",r.repo,r.path,r.symbol,r.role),r); std::size_t modify=0,reference=0,transitive=0; for(auto const&[_,r]:pending){if(r.role=="modify")++modify;else if(r.role=="reference")++reference;else ++transitive;
  } auto tx=conn.begin_transaction(db::lock_mode::immediate);if(!tx)return std::unexpected(error::query_failed);auto del=conn.prepare("delete from closures where task_id=? and extractor_version=?");if(!del||!del->bind_int64(1,task_id)||!del->bind_text(2,extractor_version)||!del->step())return std::unexpected(error::query_failed);auto ins=conn.prepare("insert into closures(task_id,repo_id,path,symbol,role,token_weight,extractor_version) values(?,?,?,?,?,?,?)");if(!ins)return std::unexpected(error::query_failed);for(auto const&[_,r]:pending){if(!ins->bind_int64(1,task_id)||!ins->bind_int64(2,r.repo)||!ins->bind_text(3,r.path)||!ins->bind_text(4,r.symbol)||!ins->bind_text(5,r.role)||!ins->bind_int64(6,r.weight)||!ins->bind_text(7,extractor_version)||!ins->step())return std::unexpected(error::query_failed);if(!ins->reset())return std::unexpected(error::query_failed);}if(!tx->commit())return std::unexpected(error::query_failed);return result{task_id,all.size(),modify,reference,transitive,pending.size()};
}
}

#include <catch2/catch_test_macros.hpp>
import std;
import planar.db;
import planar.db.migrate;
import planar.engine.closure.compute;
namespace {
struct fixture {
  std::filesystem::path root = std::filesystem::temp_directory_path()/std::format("planar_closure_compute_{}",std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::path db = root/"state.db";
  fixture(){std::filesystem::create_directories(root/"sub");}
  ~fixture(){std::error_code e;std::filesystem::remove_all(root,e);}
};
auto sql(planar::db::connection& c,std::string_view q){REQUIRE(c.execute(q));}
auto count(planar::db::connection& c,std::string_view q){auto s=c.prepare(q);REQUIRE(s);REQUIRE(s->step());return s->column_int64(0);}
}
TEST_CASE("closure compute recursively indexes direct and transitive corpus symbols and replaces its rows", "[closure][compute]") {
  fixture f; {std::ofstream{f.root/"seed.zig"} << "const ee = @import(\"ee.zig\"); pub fn run() void { ee.transform(); }\n";
  std::ofstream{f.root/"ee.zig"} << "pub fn transform() void { helper(); } fn helper() void {}\n";
  std::ofstream{f.root/".hidden.zig"} << "pub fn invisible() void {}\n";
  std::ofstream{f.root/"sub"/"outside.zig"} << "pub fn outside() void {}\n";}
  auto c=planar::db::connection::open(f.db.string());REQUIRE(c);REQUIRE(planar::db::apply_all(*c));
  sql(*c,std::format("insert into projects(id,slug,name,root_path) values(1,'r','r','{}')",f.root.string()));sql(*c,"insert into plans(id,scope_kind,title,slug,status) values(1,'global','p','p','draft')");sql(*c,"insert into tasks(id,scope_kind,plan_id,title,slug,status) values(1,'global',1,'t','t','todo')");sql(*c,"insert into task_touch_paths(task_id,repo_id,path) values(1,1,'seed.zig')");
  auto first=planar::engine::closure::compute::run(*c,1);REQUIRE(first);CHECK(first->reference>=1);CHECK(first->transitive>=1);CHECK(count(*c,"select count(*) from closures where symbol='ee.transform' and role='reference'")==1);CHECK(count(*c,"select count(*) from closures where symbol='ee.helper' and role='transitive'")==1);CHECK(count(*c,"select count(*) from closures where symbol like 'outside.%' or symbol like 'hidden.%'")==0);
  auto second=planar::engine::closure::compute::run(*c,1);REQUIRE(second);CHECK(count(*c,"select count(*) from closures where task_id=1 and extractor_version='m2-closure-0.1'")==static_cast<std::int64_t>(second->rows_written));
}

#include <catch2/catch_test_macros.hpp>
import std;
import planar.db;
import planar.db.migrate;
import planar.engine.closure.compute;
namespace {
struct fixture {
  std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      std::format("planar_closure_compute_{}", std::chrono::steady_clock::now().time_since_epoch().count());
  std::filesystem::path db = root / "state.db";
  fixture() {
    std::filesystem::create_directories(root / "sub");
  }
  ~fixture() {
    std::error_code e;
    std::filesystem::remove_all(root, e);
  }
};
auto sql(planar::db::connection& c, std::string_view q) {
  REQUIRE(c.execute(q));
}
auto count(planar::db::connection& c, std::string_view q) {
  auto s = c.prepare(q);
  REQUIRE(s);
  REQUIRE(s->step());
  return s->column_int64(0);
}
} // namespace
TEST_CASE("closure compute recursively indexes direct and transitive corpus symbols and replaces its rows",
          "[closure][compute]") {
  fixture f;
  {
    std::ofstream{f.root / "seed.zig"} << "const ee = @import(\"ee.zig\"); const widget = @import(\"widget.zig\"); const hidden "
                                          "= @import(\".hidden.zig\"); pub fn run() "
                                          "void { ee.transform(); var w: widget.Foo = .{}; w.bar(); hidden.invisible(); }\n";
    std::ofstream{f.root / "ee.zig"} << "pub fn transform() void { helper(); } fn helper() void {}\n";
    std::ofstream{f.root / "widget.zig"}
        << "pub const Foo = struct { pub fn bar(self: Foo) void {} pub fn baz(self: Foo) void {} };\n";
    std::ofstream{f.root / ".hidden.zig"} << "pub fn invisible() void {}\n";
    std::ofstream{f.root / "sub" / "outside.zig"} << "pub fn outside() void {}\n";
  }
  auto c = planar::db::connection::open(f.db.string());
  REQUIRE(c);
  REQUIRE(planar::db::apply_all(*c));
  sql(*c, std::format("insert into projects(id,slug,name,root_path) values(1,'r','r','{}')", f.root.string()));
  sql(*c, "insert into plans(id,scope_kind,title,slug,status) values(1,'global','p','p','draft')");
  sql(*c, "insert into tasks(id,scope_kind,plan_id,title,slug,status) values(1,'global',1,'t','t','todo')");
  sql(*c, "insert into task_touch_paths(task_id,repo_id,path) values(1,1,'seed.zig')");
  auto first = planar::engine::closure::compute::run(*c, 1);
  REQUIRE(first);
  CHECK(first->reference >= 1);
  CHECK(first->transitive >= 1);
  CHECK(count(*c, "select count(*) from closures where symbol='ee.transform' and role='reference'") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='ee.helper' and role='transitive'") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='widget.Foo.bar' and role='reference'") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='widget.Foo.baz'") == 0);
  // This is intentionally referenced: indexing a dot-file would otherwise
  // look harmless because an unreferenced definition produces no row.
  CHECK(count(*c, "select count(*) from closures where symbol like 'outside.%' or symbol like 'hidden.%'") == 0);
  auto second = planar::engine::closure::compute::run(*c, 1);
  REQUIRE(second);
  CHECK(count(*c, "select count(*) from closures where task_id=1 and extractor_version='m2-closure-0.1'") ==
        static_cast<std::int64_t>(second->rows_written));
}

TEST_CASE("closure compute keeps self and typed receivers in their lexical containers", "[closure][compute]") {
  fixture f;
  std::ofstream{f.root / "seed.zig"}
      << "const widget = @import(\"widget.zig\"); pub const One = struct { pub fn run(self: One) void { self.hit(); } pub fn "
         "hit(self: One) void {} }; "
         "pub const Two = struct { pub fn run(self: Two) void { self.hit(); } pub fn hit(self: Two) void {} }; "
         "pub fn use() void { var item: widget.First = .{}; item.hit(); { var item: widget.Second = .{}; item.hit(); } }\n";
  std::ofstream{f.root / "widget.zig"} << "pub const First = struct { pub fn hit(self: First) void {} }; pub const Second = "
                                          "struct { pub fn hit(self: Second) void {} };\n";
  auto c = planar::db::connection::open(f.db.string());
  REQUIRE(c);
  REQUIRE(planar::db::apply_all(*c));
  sql(*c, std::format("insert into projects(id,slug,name,root_path) values(1,'r','r','{}')", f.root.string()));
  sql(*c, "insert into plans(id,scope_kind,title,slug,status) values(1,'global','p','p','draft')");
  sql(*c, "insert into tasks(id,scope_kind,plan_id,title,slug,status) values(1,'global',1,'t','t','todo')");
  sql(*c, "insert into task_touch_paths(task_id,repo_id,path) values(1,1,'seed.zig')");
  REQUIRE(planar::engine::closure::compute::run(*c, 1));
  // Removing lexical self qualification or selecting the first typed `item`
  // declaration makes at least one of these independently reachable rows fail.
  CHECK(count(*c, "select count(*) from closures where symbol='seed.One.hit' and role='reference'") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='seed.Two.hit' and role='reference'") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='widget.First.hit' and role='reference'") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='widget.Second.hit' and role='reference'") == 1);
}

TEST_CASE("closure compute weights match Zig tokenizer comment operator and invalid-token tags", "[closure][compute]") {
  fixture f;
  std::ofstream{f.root / "seed.zig"}
      << "pub fn operators() void { /// doc\n var x: u8 = 8; x >>= 1; // ordinary comment\n _ = x; }\n"
         "pub fn invalid() void { _ = \x01; }\n";
  auto c = planar::db::connection::open(f.db.string());
  REQUIRE(c);
  REQUIRE(planar::db::apply_all(*c));
  sql(*c, std::format("insert into projects(id,slug,name,root_path) values(1,'r','r','{}')", f.root.string()));
  sql(*c, "insert into plans(id,scope_kind,title,slug,status) values(1,'global','p','p','draft')");
  sql(*c, "insert into tasks(id,scope_kind,plan_id,title,slug,status) values(1,'global',1,'t','t','todo')");
  sql(*c, "insert into task_touch_paths(task_id,repo_id,path) values(1,1,'seed.zig')");
  REQUIRE(planar::engine::closure::compute::run(*c, 1));
  // Captured from std.zig.Tokenizer: docs count, ordinary comments do not,
  // `>>=` is one tag, and the control byte is one invalid tag.
  CHECK(count(*c, "select token_weight from closures where symbol='seed.operators' and role='modify'") == 24);
  CHECK(count(*c, "select token_weight from closures where symbol='seed.invalid' and role='modify'") == 10);
}

TEST_CASE("closure compute normalizes decorated and imported-container receivers and keeps self members lexical",
          "[closure][compute]") {
  fixture f;
  std::ofstream{f.root / "seed.zig"}
      << "const widget = @import(\"widget.zig\"); const Bare = @import(\"widget.zig\").Box; "
         "pub fn run() void { var pointer: *widget.Box = undefined; pointer.hit(); var optional: ?widget.Box = null; "
         "optional.?.hit(); var generic: widget.Box(u8) = undefined; generic.hit(); var bare: *Bare = undefined; bare.hit(); }\n";
  std::ofstream{f.root / "widget.zig"}
      << "pub const Box = struct { pub fn hit(self: *const Box) void { self.mutate(); } pub fn mutate(self: ?Box) void {} "
         "pub fn miss(self: Box) void {} };\n"
         "pub const Other = struct { pub fn mutate(self: Other) void {} };\n";
  auto c = planar::db::connection::open(f.db.string());
  REQUIRE(c);
  REQUIRE(planar::db::apply_all(*c));
  sql(*c, std::format("insert into projects(id,slug,name,root_path) values(1,'r','r','{}')", f.root.string()));
  sql(*c, "insert into plans(id,scope_kind,title,slug,status) values(1,'global','p','p','draft')");
  sql(*c, "insert into tasks(id,scope_kind,plan_id,title,slug,status) values(1,'global',1,'t','t','todo')");
  sql(*c, "insert into task_touch_paths(task_id,repo_id,path) values(1,1,'seed.zig')");
  REQUIRE(planar::engine::closure::compute::run(*c, 1));
  // Removing any pointer, optional, generic, or imported alias normalization
  // leaves this row absent; every receiver form above calls `hit`.
  CHECK(count(*c, "select count(*) from closures where symbol='widget.Box.hit' and role='reference'") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='widget.Box.mutate' and role='transitive'") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='widget.Other.mutate'") == 0);
  CHECK(count(*c, "select count(*) from closures where symbol='widget.Box.miss'") == 0);
}

TEST_CASE("closure compute preserves defining repo provenance and Zig tokenizer weights", "[closure][compute]") {
  fixture    f;
  auto const one = f.root / "one";
  auto const two = f.root / "two";
  std::filesystem::create_directories(one);
  std::filesystem::create_directories(two);
  std::ofstream{one / "seed.zig"} << "const lib = @import(\"lib.zig\"); pub fn run() void { lib.call(); }\n";
  std::ofstream{one / "lib.zig"} << "pub fn call() void {}\n";
  std::ofstream{two / "seed.zig"} << "const lib = @import(\"lib.zig\"); pub fn run() void {}\n";
  std::ofstream{two / "lib.zig"} << "pub fn call() void {}\n";
  auto c = planar::db::connection::open(f.db.string());
  REQUIRE(c);
  REQUIRE(planar::db::apply_all(*c));
  sql(*c, std::format("insert into projects(id,slug,name,root_path) values(1,'one','one','{}'),(2,'two','two','{}')",
                      one.string(), two.string()));
  sql(*c, "insert into plans(id,scope_kind,title,slug,status) values(1,'global','p','p','draft')");
  sql(*c, "insert into tasks(id,scope_kind,plan_id,title,slug,status) values(1,'global',1,'t','t','todo')");
  sql(*c, "insert into task_touch_paths(task_id,repo_id,path) values(1,1,'seed.zig'),(1,2,'seed.zig')");
  REQUIRE(planar::engine::closure::compute::run(*c, 1));
  CHECK(count(*c, "select count(*) from closures where symbol='lib.call' and role='reference' and repo_id=1") == 1);
  CHECK(count(*c, "select count(*) from closures where symbol='lib.call' and role='reference' and repo_id=2") == 0);
  // Zig's tokenizer counts the `pub` modifier as its own token.
  CHECK(count(*c, "select token_weight from closures where symbol='seed.run' and role='modify' and repo_id=2") == 8);
}

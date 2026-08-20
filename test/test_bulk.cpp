/* Getting rows in: the loader that makes a table and the appender that
 * adds to one that already exists. */
#include <zu.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

/* ---- the loader ---- */

ZU_TEST(a_load_makes_a_table_that_can_be_queried) {
  zt::TempDir dir("load");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto r = conn.query("MATCH (p:Person) RETURN p.id AS id, p.name AS name ORDER BY p.id");
  CHECK_EQ(r.rows(), 3u);
  CHECK_EQ(r.ints("id")[0], 1);
  CHECK_EQ(r.str(1, r.column("name")), "grace");
}

ZU_TEST(the_loader_takes_a_vector_as_readily_as_an_array) {
  /* A caller holding std::vector<std::string> should not have to build
   * a vector of views first, which is what the range constraint on
   * strings is for. */
  zt::TempDir dir("vectors");
  const std::string path = dir.file("v.zu");
  {
    auto loader = zu::Loader::create(path);
    loader.table("Person", "Knows", 2);
    const std::vector<std::int64_t> ids{10, 20};
    const std::vector<std::string> names{"ada", "grace"};
    loader.ints("id", ids).strings("name", names).finish();
  }

  auto conn = zu::Connection::open(path);
  auto r = conn.query("MATCH (p:Person) RETURN p.id AS id ORDER BY p.id");
  CHECK_EQ(r.rows(), 2u);
  CHECK_EQ(r.ints("id")[1], 20);
}

ZU_TEST(every_column_kind_the_loader_takes) {
  zt::TempDir dir("kinds");
  const std::string path = dir.file("things.zu");
  zt::everything(path);

  auto conn = zu::Connection::open(path);
  auto r = conn.query(
      "MATCH (t:Thing) RETURN t.n AS n, t.d AS d, t.ok AS ok, t.s AS s, t.born AS born "
      "ORDER BY t.n");
  CHECK_EQ(r.rows(), 2u);
  CHECK_EQ(r.type(0, r.column("n")), zu::Type::integer);
  CHECK_EQ(r.type(0, r.column("d")), zu::Type::floating);
  CHECK_EQ(r.type(0, r.column("ok")), zu::Type::boolean);
  CHECK_EQ(r.type(0, r.column("s")), zu::Type::string);
  CHECK_EQ(r.type(0, r.column("born")), zu::Type::temporal);
}

ZU_TEST(edges_go_in_beside_the_nodes) {
  zt::TempDir dir("edges");
  const std::string path = dir.file("follows.zu");
  {
    auto loader = zu::Loader::create(path);
    loader.table("User", "Follows", 3);
    const std::array<std::int64_t, 3> ids{1, 2, 3};
    loader.ints("id", ids);
    const std::array<std::uint32_t, 2> from{0, 1};
    const std::array<std::uint32_t, 2> to{1, 2};
    loader.edges(from, to).finish();
  }

  auto conn = zu::Connection::open(path);
  auto r = conn.query("MATCH (a:User)-[:Follows]->(b:User) RETURN a.id AS src, b.id AS dst ORDER BY a.id");
  CHECK_EQ(r.rows(), 2u);
  CHECK_EQ(r.ints("src")[0], 1);
  CHECK_EQ(r.ints("dst")[0], 2);
  CHECK_EQ(r.ints("src")[1], 2);
  CHECK_EQ(r.ints("dst")[1], 3);
}

ZU_TEST(two_edge_arrays_of_different_lengths_are_refused_before_the_engine_sees_them) {
  zt::TempDir dir("mismatch");
  auto loader = zu::Loader::create(dir.file("bad.zu"));
  loader.table("User", "Follows", 2);
  const std::array<std::uint32_t, 2> from{0, 1};
  const std::array<std::uint32_t, 1> to{1};
  CHECK_THROWS_AS(zu::Exception, loader.edges(from, to));
}

ZU_TEST(a_column_of_the_wrong_length_is_refused) {
  zt::TempDir dir("short");
  auto loader = zu::Loader::create(dir.file("short.zu"));
  loader.table("Person", "Knows", 3);
  const std::array<std::int64_t, 2> ids{1, 2};
  CHECK_THROWS_AS(zu::Exception, loader.ints("id", ids));
}

/* ---- the appender ---- */

ZU_TEST(rows_go_in_and_come_back) {
  zt::TempDir dir("append");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  {
    auto rows = conn.appender("Person");
    rows.append(std::int64_t{4}).append("hedy").end_row();
    rows.append(std::int64_t{5}).append("katherine").end_row();
    CHECK_EQ(rows.close(), 2u);
  }

  auto r = conn.query("MATCH (p:Person) RETURN p.name AS name ORDER BY p.id");
  CHECK_EQ(r.rows(), 5u);
  CHECK_EQ(r.str(3, 0), "hedy");
  CHECK_EQ(r.str(4, 0), "katherine");
}

ZU_TEST(a_whole_row_goes_in_at_once) {
  zt::TempDir dir("rowat");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  {
    auto rows = conn.appender("Person");
    rows.row(std::int64_t{4}, "hedy");
    rows.row(std::int64_t{5}, std::string_view("katherine"));
    CHECK_EQ(rows.close(), 2u);
  }
  CHECK_EQ(conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int(), 5);
}

ZU_TEST(a_row_is_a_row_once_end_row_has_ended_it) {
  zt::TempDir dir("ended");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto rows = conn.appender("Person");
  CHECK_EQ(rows.buffered(), 0u);
  rows.append(std::int64_t{4}).append("hedy");
  CHECK_EQ(rows.buffered(), 0u);
  rows.end_row();
  CHECK_EQ(rows.buffered(), 1u);
  CHECK_EQ(rows.committed(), 0u);
  rows.flush();
  CHECK_EQ(rows.buffered(), 0u);
  CHECK_EQ(rows.committed(), 1u);
}

ZU_TEST(the_appender_says_what_it_is_writing) {
  zt::TempDir dir("cols");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto rows = conn.appender("Person");
  CHECK_EQ(rows.cols(), 2u);
  CHECK_EQ(rows.col_name(0), "id");
  CHECK_EQ(rows.col_name(1), "name");
  CHECK_THROWS_AS(zu::Exception, rows.col_name(2));
}

ZU_TEST(what_is_discarded_never_arrives) {
  zt::TempDir dir("discard");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  {
    auto rows = conn.appender("Person");
    rows.append(std::int64_t{4}).append("hedy").end_row();
    rows.flush();
    rows.append(std::int64_t{5}).append("katherine").end_row();
    CHECK_EQ(rows.discard(), 1u);
    CHECK_EQ(rows.buffered(), 0u);
    /* A discard does not reach back to what an earlier flush wrote. */
    CHECK_EQ(rows.committed(), 1u);
  }
  CHECK_EQ(conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int(), 4);
}

ZU_TEST(closing_without_finishing_keeps_what_was_written) {
  /* A loop that threw halfway keeps the rows it managed. Throwing away
   * work that succeeded is not a decision a destructor gets to make. */
  zt::TempDir dir("kept");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  {
    auto rows = conn.appender("Person");
    rows.append(std::int64_t{4}).append("hedy").end_row();
  }
  CHECK_EQ(conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int(), 4);
}

ZU_TEST(closing_twice_writes_nothing_the_second_time) {
  zt::TempDir dir("twice");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto rows = conn.appender("Person");
  rows.append(std::int64_t{4}).append("hedy").end_row();
  CHECK_EQ(rows.close(), 1u);
  /* The same total again rather than a fresh count, because what close
   * answers is the rows it committed in all and closing a spent
   * appender commits nothing more. */
  CHECK_EQ(rows.close(), 1u);
  CHECK_EQ(conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int(), 4);
}

ZU_TEST(a_value_of_the_wrong_type_ends_its_row_and_not_the_appender) {
  zt::TempDir dir("wrong");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto rows = conn.appender("Person");
  CHECK_THROWS_AS(zu::Exception, rows.append("not an id"));
  /* The appender is still usable once the loop is fixed. */
  rows.append(std::int64_t{4}).append("hedy").end_row();
  CHECK_EQ(rows.close(), 1u);
  CHECK_EQ(conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int(), 4);
}

ZU_TEST(an_appender_for_a_table_that_is_not_there_is_refused) {
  zt::TempDir dir("nosuch");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  CHECK_THROWS_AS(zu::Exception, conn.appender("Nobody"));
}

ZU_TEST_MAIN()

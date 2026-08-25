/* Asking a question and reading the answer, which is what almost every
 * program that uses this ever does. */
#include <zu.hpp>

#include <algorithm>
#include <numeric>
#include <ranges>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

ZU_TEST(a_database_in_memory_answers_a_statement) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS one");
  CHECK_EQ(r.rows(), 1u);
  CHECK_EQ(r.cols(), 1u);
  CHECK_EQ(r.name(0), "one");
  CHECK_EQ(r.cell(0, 0).as_int(), 1);
}

ZU_TEST(the_wrapper_and_the_library_agree_about_the_abi) {
  CHECK_EQ(zu::abi_version(), "0.15");
  CHECK(!zu::version().empty());
}

ZU_TEST(a_database_and_its_connections_are_separate_things) {
  auto db = zu::Database::memory();
  CHECK(db.is_memory());
  CHECK(!db.path().empty());

  auto first = db.connect();
  auto second = db.connect();
  CHECK_EQ(first.query("RETURN 1 AS v").cell(0, 0).as_int(), 1);
  CHECK_EQ(second.query("RETURN 2 AS v").cell(0, 0).as_int(), 2);
}

ZU_TEST(a_connection_duplicates_itself_without_a_path) {
  auto conn = zu::Connection::memory();
  auto other = conn.duplicate();
  CHECK_EQ(other.query("RETURN 1 AS v").cell(0, 0).as_int(), 1);
  /* Two connections and one graph, which is what makes a duplicate
   * worth having over a second open. */
  CHECK_EQ(conn.query("RETURN 1 AS v").cell(0, 0).as_int(), 1);
}

ZU_TEST(the_shorthand_opens_a_database_and_a_connection_at_once) {
  zt::TempDir dir("shorthand");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  CHECK_EQ(conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int(), 3);
}

ZU_TEST(a_result_is_a_range_of_rows) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [10, 20, 30] AS v RETURN v");

  std::vector<std::int64_t> read;
  for (auto row : r) {
    read.push_back(row.get<std::int64_t>("v"));
  }
  CHECK_EQ(read.size(), 3u);
  CHECK_EQ(read[0], 10);
  CHECK_EQ(read[1], 20);
  CHECK_EQ(read[2], 30);
}

ZU_TEST(the_standard_algorithms_work_over_a_result) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2, 3, 4, 5] AS v RETURN v");

  CHECK_EQ(std::ranges::size(r), 5u);
  CHECK(!r.empty());

  std::int64_t total = 0;
  for (auto row : r | std::views::take(3)) {
    total += row.get<std::int64_t>(0);
  }
  CHECK_EQ(total, 6);

  /* A random access range rather than an input one, which is what lets
   * the last row be read without walking to it. */
  const auto last = *(r.begin() + 4);
  CHECK_EQ(last.get<std::int64_t>(0), 5);
  CHECK_EQ(r.end() - r.begin(), 5);
}

ZU_TEST(a_row_reads_a_column_by_name_or_by_number) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS i, 1.5 AS f, 'ada' AS s, true AS b, null AS n");
  CHECK_EQ(r.cols(), 5u);

  const auto row = r.row(0);
  CHECK_EQ(row.get<std::int64_t>("i"), 1);
  CHECK_EQ(row.get<double>("f"), 1.5);
  CHECK_EQ(row.get<std::string_view>("s"), "ada");
  CHECK_EQ(row.get<bool>("b"), true);
  CHECK(row.is_null("n"));

  CHECK_EQ(row.get<std::int64_t>(0), 1);
  CHECK_EQ(row.get<std::string>(2), std::string("ada"));
}

ZU_TEST(a_nullable_column_reads_as_an_optional) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, null, 3] AS v RETURN v");
  CHECK_EQ(r.rows(), 3u);
  CHECK_EQ(r.row(0).get<std::optional<std::int64_t>>(0).value_or(-1), 1);
  CHECK(!r.row(1).get<std::optional<std::int64_t>>(0).has_value());
  CHECK_EQ(r.row(2).get<std::optional<std::int64_t>>(0).value_or(-1), 3);
}

ZU_TEST(a_column_is_found_by_name_and_says_so_when_it_is_not_there) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS one, 'x' AS two");
  CHECK_EQ(r.column("one"), 0u);
  CHECK_EQ(r.column("two"), 1u);
  CHECK(!r.find("three").has_value());
  CHECK_THROWS_AS(zu::ProgrammingError, r.column("three"));
  CHECK_EQ(r.names().size(), 2u);
  CHECK_EQ(r.names()[1], "two");
}

ZU_TEST(a_string_cell_is_borrowed_from_the_result) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND ['ada', 'grace'] AS v RETURN v");
  const std::string_view first = r.str(0, 0);
  const std::string_view second = r.str(1, 0);
  CHECK_EQ(first, "ada");
  CHECK_EQ(second, "grace");
  /* NUL-terminated, which is what the C API promises and what makes a
   * view of one safe to hand to something that wants a C string. */
  CHECK_EQ(first.data()[first.size()], '\0');
}

ZU_TEST(a_statement_that_answers_nothing_is_not_a_failure) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [] AS v RETURN v");
  CHECK_EQ(r.rows(), 0u);
  CHECK(r.empty());
  CHECK(r.begin() == r.end());
  CHECK(r.ints(0).empty());
}

ZU_TEST(every_result_carries_a_completion_condition) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS one");
  CHECK_EQ(r.gqlstatus().size(), 5u);
  CHECK_EQ(r.notice_count(), 0u);
  CHECK(r.notices().empty());
}

ZU_TEST(a_result_outlives_the_connection_that_made_it) {
  /* Rows are the result's own, so a pooled connection can go back to
   * the pool while the caller is still reading. */
  zu::Result r;
  {
    auto conn = zu::Connection::memory();
    r = conn.query("UNWIND [1, 2, 3] AS v RETURN v");
  }
  CHECK_EQ(r.rows(), 3u);
  CHECK_EQ(r.ints(0)[2], 3);
}

ZU_TEST(the_engine_reads_a_table_that_was_loaded_into_a_file) {
  zt::TempDir dir("file");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto db = zu::Database::open(path);
  CHECK(!db.is_memory());
  auto conn = db.connect();
  auto r = conn.query("MATCH (p:Person) RETURN p.id AS id, p.name AS name ORDER BY p.id");
  CHECK_EQ(r.rows(), 3u);
  CHECK_EQ(r.row(0).get<std::string_view>("name"), "ada");
  CHECK_EQ(r.row(2).get<std::int64_t>("id"), 3);
}

ZU_TEST(a_config_is_read_and_a_bad_option_is_named) {
  zu::Config cfg;
  cfg.memory_limit(64u << 20).threads(1).read_only(false);
  cfg.set("threads", "2");
  CHECK_THROWS_AS(zu::Exception, cfg.set("nonsense", "1"));

  auto db = zu::Database::memory(cfg);
  auto conn = db.connect();
  CHECK_EQ(conn.query("RETURN 1 AS v").cell(0, 0).as_int(), 1);
}

ZU_TEST_MAIN()

/* The other half of the error model.
 *
 * Every fallible call has a try_ spelling that answers std::expected
 * rather than throwing, and the two are the same code underneath, so
 * what these cases check is that the two agree: what throws returns an
 * error, what succeeds returns a value, and the error carries the same
 * GQLSTATUS either way.
 *
 * The whole file compiles to nothing under C++20, because std::expected
 * arrived in 23 and the throwing half is complete on its own. */
#include <zu.hpp>

#include "fixture.hpp"
#include "harness.hpp"

#if ZU_HAS_EXPECTED

#include <string>
#include <vector>

ZU_TEST(the_expected_spelling_answers_a_value_when_the_call_works) {
  auto conn = zu::Connection::try_memory();
  CHECK(conn.has_value());

  auto r = conn->try_query("RETURN 1 AS one");
  CHECK(r.has_value());
  CHECK_EQ(r->rows(), 1u);

  auto cell = r->try_cell(0, 0);
  CHECK(cell.has_value());
  CHECK_EQ(cell->try_as_int().value(), 1);
}

ZU_TEST(the_expected_spelling_answers_an_error_where_the_other_throws) {
  auto conn = zu::Connection::memory();
  auto r = conn.try_query("RETURN RETURN");
  CHECK(!r.has_value());
  CHECK_EQ(r.error().status(), zu::Status::error);
  CHECK(r.error().code().has_value());
  CHECK_EQ(r.error().code()->substr(0, 2), std::string_view("42"));
  CHECK(!r.error().message().empty());
}

ZU_TEST(both_spellings_carry_the_same_error) {
  auto conn = zu::Connection::memory();
  std::string thrown_code;
  try {
    conn.query("RETURN RETURN");
  } catch (const zu::Exception& e) {
    thrown_code = std::string(e.code().value_or(""));
  }
  const auto returned = conn.try_query("RETURN RETURN");
  CHECK(!returned.has_value());
  CHECK_EQ(std::string(returned.error().code().value_or("")), thrown_code);
}

ZU_TEST(a_void_call_answers_expected_of_void) {
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $v AS v");
  const auto bound = stmt.try_bind("v", std::int64_t{3});
  CHECK(bound.has_value());
  auto r = stmt.try_execute();
  CHECK(r.has_value());
  CHECK_EQ(r->row(0).get<std::int64_t>(0), 3);
}

ZU_TEST(the_expected_spelling_chains_with_and_then) {
  auto conn = zu::Connection::memory();
  const auto answer =
      conn.try_query("RETURN 41 AS v")
          .and_then([](zu::Result r) -> zu::expected<std::int64_t> {
            return r.try_cell(0, 0).and_then([](zu::Value v) { return v.try_as_int(); });
          })
          .transform([](std::int64_t v) { return v + 1; });
  CHECK(answer.has_value());
  CHECK_EQ(*answer, 42);
}

ZU_TEST(a_failure_short_circuits_a_chain) {
  auto conn = zu::Connection::memory();
  const auto answer = conn.try_query("RETURN RETURN").transform([](zu::Result r) {
    return r.rows();
  });
  CHECK(!answer.has_value());
  CHECK_EQ(answer.error().status(), zu::Status::error);
}

ZU_TEST(a_column_that_is_not_there_returns_rather_than_throws) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS one");
  const auto col = r.try_column("two");
  CHECK(!col.has_value());
  CHECK_EQ(col.error().status(), zu::Status::misuse);

  const auto found = r.try_column("one");
  CHECK(found.has_value());
  CHECK_EQ(*found, 0u);
}

ZU_TEST(the_bulk_paths_have_the_expected_spelling_too) {
  zt::TempDir dir("expected");
  const std::string path = dir.file("people.zu");
  {
    auto loader = zu::Loader::try_create(path);
    CHECK(loader.has_value());
    CHECK(loader->try_table("Person", "Knows", 2).has_value());
    const std::vector<std::int64_t> ids{1, 2};
    const std::vector<std::string> names{"ada", "grace"};
    CHECK(loader->try_ints("id", ids).has_value());
    CHECK(loader->try_strings("name", names).has_value());
    CHECK(loader->try_finish().has_value());
  }

  auto conn = zu::Connection::try_open(path);
  CHECK(conn.has_value());
  auto rows = conn->try_appender("Person");
  CHECK(rows.has_value());
  CHECK(rows->try_append(std::int64_t{3}).has_value());
  CHECK(rows->try_append(std::string_view("alan")).has_value());
  CHECK(rows->try_end_row().has_value());
  const auto closed = rows->try_close();
  CHECK(closed.has_value());
  CHECK_EQ(*closed, 1u);

  const auto missing = conn->try_appender("Nobody");
  CHECK(!missing.has_value());
}

ZU_TEST(a_transaction_begins_and_commits_without_throwing) {
  auto conn = zu::Connection::memory();
  auto tx = conn.try_transaction();
  CHECK(tx.has_value());
  CHECK(conn.in_transaction());
  CHECK(conn.try_commit().has_value());
  CHECK(!conn.in_transaction());
  /* The guard was spent by the bare commit, so letting it go does not
   * roll back a transaction that is not there. */
  CHECK(!conn.try_commit().has_value());
}

ZU_TEST(a_frame_registers_without_throwing) {
  const std::vector<std::int64_t> ids{1, 2, 3};
  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::try_create("Person", ids.size());
  CHECK(frame.has_value());
  CHECK(frame->try_column("id", ids).has_value());
  CHECK(conn.try_register_frame(*frame).has_value());
  const auto names = conn.try_registered();
  CHECK(names.has_value());
  CHECK_EQ(names->size(), 1u);
  CHECK_EQ((*names)[0], std::string("Person"));
}

#else

ZU_TEST(this_toolchain_has_no_std_expected_and_the_throwing_half_is_enough) {
  auto conn = zu::Connection::memory();
  CHECK_EQ(conn.query("RETURN 1 AS one").rows(), 1u);
}

#endif /* ZU_HAS_EXPECTED */

ZU_TEST_MAIN()

/* The other half of the error model.
 *
 * Every fallible call has a try_ spelling that answers std::expected
 * rather than throwing, and the two are the same code underneath, so
 * what these cases check is that the two agree: what throws returns an
 * error, what succeeds returns a value, and the error carries the same
 * GQLSTATUS either way.
 *
 * The whole file compiles to nothing on a toolchain with no
 * std::expected, which is every C++20 build and also clang against
 * libstdc++, where the two disagree about concepts and the C++23
 * library never switches on. The throwing half is complete on its own,
 * so that is a narrower API rather than a broken one. */
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

ZU_TEST(the_calls_abi_0_14_added_have_the_expected_spelling_too) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN X'00AB' AS b, 'ada' AS s");

  const auto b = r.cell(0, 0).try_as_bytes();
  CHECK(b.has_value());
  CHECK_EQ(b->size(), 2u);
  /* Text read as octets is a mistake in the program rather than a
   * failure of the engine, and here it comes back rather than throws. */
  const auto wrong = r.cell(0, 1).try_as_bytes();
  CHECK(!wrong.has_value());
  CHECK_EQ(wrong.error().status(), zu::Status::misuse);

  /* Two layers of nothing, and they mean different things: the outer
   * one is the call having failed, the inner one is no table having
   * that id. */
  const auto absent = conn.try_table_name(9999);
  CHECK(absent.has_value());
  CHECK(!absent->has_value());
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

/* A file that compiles to nothing is a file that passes for the wrong
 * reason, and this one did.
 *
 * zu.hpp used to read __cpp_lib_expected before <version> had defined
 * it, so ZU_HAS_EXPECTED came out 0 on GCC 13 at -std=c++23 and every
 * case above compiled away. What ran instead was one placeholder that
 * queries a number, ctest wrote "Passed", and twelve cases about the
 * half of the API a caller who builds without exceptions depends on
 * had not been compiled anywhere for months.
 *
 * So the floor build says so out loud, and what it asks is the
 * question the bug was: does this library have std::expected, and did
 * zu.hpp fail to see it. Not whether the standard is C++23, which is a
 * different question with a different answer. Clang reports
 * __cpp_concepts as 201907L rather than 202002L, and libstdc++ gates
 * every C++23 library feature on the later number, so clang against
 * libstdc++ at -std=c++23 has no std::expected at all. That is a
 * toolchain without it and not a header that mislaid it, and asking
 * about the standard alone called it the second.
 *
 * <version> is included here rather than left to zu.hpp, because zu.hpp
 * having included it is the thing being checked. If it stops, the macro
 * is undefined while zu.hpp reads it and defined by the time this does,
 * which is the whole of the original bug and is what fires this. */
#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202202L
#error "std::expected is in this library and zu.hpp turned the try_ half off anyway. Check that <version> is included before the feature tests in zu.hpp rather than deleting this line."
#endif

ZU_TEST(this_toolchain_has_no_std_expected_and_the_throwing_half_is_enough) {
  auto conn = zu::Connection::memory();
  CHECK_EQ(conn.query("RETURN 1 AS one").rows(), 1u);
}

#endif /* ZU_HAS_EXPECTED */

ZU_TEST_MAIN()

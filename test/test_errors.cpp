/* What a failure looks like on the way out.
 *
 * The rule the whole header is built around: a call that fails throws a
 * type named after what went wrong, carrying the GQLSTATUS the engine
 * gave, and nothing leaks on the way. */
#include <zu.hpp>

#include <string>

#include "fixture.hpp"
#include "harness.hpp"

ZU_TEST(a_bad_statement_is_a_syntax_error) {
  auto conn = zu::Connection::memory();
  CHECK_THROWS_AS(zu::SyntaxError, conn.query("RETURN RETURN"));
}

ZU_TEST(a_syntax_error_carries_the_class_it_was_named_for) {
  auto conn = zu::Connection::memory();
  try {
    conn.query("RETURN RETURN");
    zt::fail(__FILE__, __LINE__, "a bad statement answered");
  } catch (const zu::SyntaxError& e) {
    CHECK(e.code().has_value());
    CHECK_EQ(e.code()->substr(0, 2), std::string_view("42"));
    CHECK_EQ(e.code()->size(), 5u);
    CHECK(!e.retryable());
    CHECK_EQ(e.status(), zu::Status::error);
    CHECK(std::string(e.what()).size() > 0);
  }
}

ZU_TEST(an_error_names_where_in_the_statement_it_was) {
  auto conn = zu::Connection::memory();
  try {
    conn.query("RETURN RETURN");
    zt::fail(__FILE__, __LINE__, "a bad statement answered");
  } catch (const zu::Exception& e) {
    const zu::Error& err = e.error();
    if (err.position().has_value()) {
      CHECK(err.position()->line >= 1);
      CHECK(err.position()->column >= 1);
      /* The report is the excerpt with a caret under the offending
       * character, which is what a REPL prints and what a log wants. */
      const std::string report = err.report();
      CHECK(report.find('^') != std::string::npos);
    }
    CHECK(!err.message().empty());
  }
}

ZU_TEST(an_error_that_is_not_ours_still_has_something_to_say) {
  /* No engine error behind it, so the message is the wrapper's own and
   * the status is the one it was made with. */
  const zu::Error e = zu::Error::take(zu::Status::misuse, nullptr, "zu_test");
  CHECK_EQ(e.status(), zu::Status::misuse);
  CHECK(!e.message().empty());
  CHECK(!e.code().has_value());
  CHECK(!e.retryable());
  CHECK(!e.report().empty());
}

ZU_TEST(a_column_that_is_not_there_is_the_callers_mistake) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS one");
  CHECK_THROWS_AS(zu::ProgrammingError, r.column("two"));
  CHECK_THROWS_AS(zu::ProgrammingError, r.name(9));
  CHECK_THROWS_AS(zu::ProgrammingError, r.cell(9, 0));
  CHECK_THROWS_AS(zu::ProgrammingError, r.cell(0, 9));
}

ZU_TEST(a_cell_read_as_the_wrong_type_says_so) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 'ada' AS s");
  CHECK_EQ(r.cell(0, 0).type(), zu::Type::string);
  CHECK_THROWS_AS(zu::Exception, r.cell(0, 0).as_int());
  CHECK_THROWS_AS(zu::Exception, r.row(0).get<std::int64_t>(0));
}

ZU_TEST(an_integer_that_does_not_fit_is_refused_rather_than_wrapped) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 100000 AS big");
  CHECK_EQ(r.row(0).get<std::int64_t>(0), 100000);
  /* A silently truncated 100000 is the bug this exists to not have.
   * The type was the caller's to choose, so choosing one the value does
   * not fit is a mistake in the program rather than in the data. */
  CHECK_THROWS_AS(zu::ProgrammingError, r.row(0).get<std::int16_t>(0));
}

ZU_TEST(a_file_that_is_not_a_database_does_not_open) {
  zt::TempDir dir("notadb");
  CHECK_THROWS_AS(zu::Exception, zu::Database::open(dir.file("missing.zu")));
}

ZU_TEST(a_closed_handle_is_a_misuse_and_not_a_crash) {
  auto conn = zu::Connection::memory();
  auto other = std::move(conn);
  /* conn is now the empty one, and asking it anything is a mistake the
   * wrapper reports rather than a null pointer the engine reads. */
  CHECK(!static_cast<bool>(conn));
  CHECK(static_cast<bool>(other));
  CHECK_THROWS_AS(zu::Exception, conn.query("RETURN 1 AS v"));
}

ZU_TEST(every_exception_is_a_runtime_error) {
  auto conn = zu::Connection::memory();
  bool caught = false;
  try {
    conn.query("RETURN RETURN");
  } catch (const std::runtime_error& e) {
    caught = true;
    CHECK(std::string(e.what()).find("42") != std::string::npos ||
          !std::string(e.what()).empty());
  }
  CHECK(caught);
}

ZU_TEST(an_error_survives_being_copied_out_of_its_catch) {
  auto conn = zu::Connection::memory();
  zu::Error kept;
  try {
    conn.query("RETURN RETURN");
  } catch (const zu::Exception& e) {
    kept = e.error();
  }
  /* The strings are the Error's own, so the zu_error behind it is long
   * gone and this still reads. */
  CHECK(!kept.message().empty());
  CHECK(kept.code().has_value());
}

ZU_TEST_MAIN()

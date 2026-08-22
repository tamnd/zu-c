/* What a refusal carries, checked rather than promised.
 *
 * The scorecard item this file exists for asks that every failure carry
 * the standard's condition code, the place, and the doc URL, and that a
 * test say so rather than a page. The reason it is an item at all is
 * that these fields are the ones nothing notices the loss of: a client
 * that drops the code still throws, still prints something, and still
 * passes every test written about what it does rather than about what
 * it says. This wrapper dropped all of them for a while and looked fine
 * doing it.
 *
 * So the cases here are about the parts, one at a time. The message is
 * barely checked, because the message is the part that is allowed to be
 * reworded; the code, the class, the doc URL and the subject are the
 * parts a program acts on and they are checked exactly.
 *
 * Every expected value in here was read off the running engine before
 * it was written down. Where the engine gives nothing, the case says so
 * and names the issue, because a gap recorded is a gap somebody can
 * close and a gap skipped is a gap nobody knows about.
 */
#include <zu.hpp>

#include <string>
#include <string_view>

#include "fixture.hpp"
#include "harness.hpp"

namespace {

/* The Error behind whatever a statement threw, or a failure saying it
 * did not throw at all. */
zu::Error refusal(zu::Connection& conn, std::string_view statement, const char* file, int line) {
  try {
    conn.query(statement);
  } catch (const zu::Exception& e) {
    return e.error();
  }
  zt::fail(file, line, "the engine answered a statement written to be refused");
}

#define REFUSAL(conn, statement) refusal((conn), (statement), __FILE__, __LINE__)

}  // namespace

ZU_TEST(a_refusal_carries_the_condition_the_class_and_the_page_it_is_written_up_on) {
  auto conn = zu::Connection::memory();
  const zu::Error e = REFUSAL(conn, "MATCH (p:Person RETURN p");

  CHECK(e.code().has_value());
  CHECK_EQ(*e.code(), std::string_view("42001"));

  /* The standard's own words, which is what a conformance harness
   * grades against and what a client has no business rewriting. */
  CHECK(e.condition().has_value());
  CHECK(e.condition()->find("syntax error") != std::string_view::npos);

  CHECK(e.doc_url().has_value());
  CHECK_EQ(*e.doc_url(), std::string_view("https://zu.dev/docs/errors/42001"));

  CHECK_EQ(e.severity(), zu::Severity::exception);
  CHECK(!e.retryable());
  CHECK_EQ(e.status(), zu::Status::error);
}

ZU_TEST(the_doc_url_ends_with_the_code_it_is_the_page_for) {
  /* The one invariant that makes a doc link worth printing. A URL that
   * is present but points at another condition is worse than none,
   * because the reader believes it. This is checked across conditions
   * from three different classes rather than on one, so a URL built
   * from a constant somewhere would show up. */
  auto conn = zu::Connection::memory();
  const char* statements[] = {
      "MATCH (p:Person RETURN p",  /* 42001, parse */
      "RETURN nope",               /* 42002, binder */
      "RETURN 1 + 'ada'",          /* 22G03, types */
      "RETURN 1 / 0",              /* 22012, arithmetic */
  };
  for (const char* s : statements) {
    const zu::Error e = REFUSAL(conn, s);
    CHECK(e.code().has_value());
    CHECK(e.doc_url().has_value());
    const std::string expected = "https://zu.dev/docs/errors/" + std::string(*e.code());
    CHECK_EQ(*e.doc_url(), std::string_view(expected));
  }
}

ZU_TEST(a_condition_the_parser_raised_says_where_in_the_statement_it_was) {
  auto conn = zu::Connection::memory();
  const zu::Error e = REFUSAL(conn, "MATCH (p:Person RETURN p");

  CHECK(e.position().has_value());
  /* One statement on one line, and the column is where RETURN starts,
   * counting from one. The offset is the same place counted in bytes
   * from zero, which is what an editor wants and what a column cannot
   * give it once there is a character outside ASCII on the line. */
  CHECK_EQ(e.position()->line, 1u);
  CHECK_EQ(e.position()->column, 17u);
  CHECK_EQ(e.position()->offset, 16u);

  CHECK(e.excerpt().has_value());
  CHECK_EQ(*e.excerpt(), std::string_view("MATCH (p:Person RETURN p"));

  /* Excerpt plus position is a caret, and the caret lands under the
   * column rather than near it. */
  const std::string report = e.report();
  const std::size_t caret = report.find('^');
  CHECK(caret != std::string::npos);
  const std::size_t line_start = report.rfind('\n', caret);
  CHECK(line_start != std::string::npos);
  CHECK_EQ(caret - line_start, std::size_t{e.position()->column});
}

ZU_TEST(the_offset_counts_bytes_where_the_column_counts_characters) {
  /* The two are the same place said two ways and they stop agreeing the
   * moment a statement has a character outside ASCII in it. A client
   * that hands an editor the column where a byte index was wanted
   * underlines the wrong word, and it underlines the right word in
   * every test written in ASCII, which is why this pair exists.
   *
   * Two statements of the same shape. The first is ASCII throughout, so
   * the offset is the column less the one it counts from. The second
   * has an earth in it, four bytes wide and one column wide, and after
   * it the offset runs ahead of the column rather than behind it. */
  auto conn = zu::Connection::memory();

  const zu::Error plain = REFUSAL(conn, "RETURN 'ab' AS one AS two");
  CHECK(plain.position().has_value());
  CHECK_EQ(plain.position()->offset, plain.position()->column - 1);

  const zu::Error wide = REFUSAL(conn, "RETURN '\xf0\x9f\x8c\x8d' AS one AS two");
  CHECK(wide.position().has_value());
  /* Three bytes of the four are columns nobody counted, so the offset
   * is three past where the ASCII arithmetic would have put it. */
  CHECK_EQ(wide.position()->offset, wide.position()->column + 2);
  CHECK(wide.position()->offset > wide.position()->column);
}

ZU_TEST(a_condition_about_a_name_says_which_name_and_what_kind_of_name) {
  /* The pair a tool acts on. Underlining the subject or suggesting a
   * near spelling means having the name as a name, and pulling it back
   * out of a sentence with quotes in it is the sort of parsing that
   * works until the sentence is reworded. */
  auto conn = zu::Connection::memory();

  const zu::Error variable = REFUSAL(conn, "RETURN nope");
  CHECK_EQ(*variable.code(), std::string_view("42002"));
  CHECK(variable.subject_kind().has_value());
  CHECK_EQ(*variable.subject_kind(), std::string_view("variable"));
  CHECK(variable.subject().has_value());
  CHECK_EQ(*variable.subject(), std::string_view("nope"));

  const zu::Error function = REFUSAL(conn, "RETURN nosuchfn(1)");
  CHECK_EQ(*function.code(), std::string_view("42002"));
  CHECK_EQ(*function.subject_kind(), std::string_view("function"));
  CHECK_EQ(*function.subject(), std::string_view("nosuchfn"));

  /* Same code, same class, different subject. That is the whole reason
   * the subject is a field: the code does not tell these apart. */
  CHECK_NE(*variable.subject(), *function.subject());
}

ZU_TEST(a_refusal_says_which_graph_and_schema_the_statement_was_running_in) {
  auto conn = zu::Connection::memory();
  const zu::Error e = REFUSAL(conn, "RETURN nope");
  CHECK(e.graph().has_value());
  CHECK_EQ(*e.graph(), std::string_view("home"));
  CHECK(e.schema().has_value());
  CHECK_EQ(*e.schema(), std::string_view("/"));
}

ZU_TEST(a_condition_with_nothing_to_point_at_carries_no_place_rather_than_a_wrong_one) {
  /* A division by zero happens while a statement runs, long after the
   * tokens are gone, so there is no token to point at. Empty is the
   * honest answer and a made up position would be worse than none: an
   * editor would underline whatever was at line one column one. */
  auto conn = zu::Connection::memory();
  const zu::Error e = REFUSAL(conn, "RETURN 1 / 0");
  CHECK_EQ(*e.code(), std::string_view("22012"));
  CHECK(e.condition().has_value());
  CHECK(e.doc_url().has_value());
  CHECK(!e.position().has_value());
  CHECK(!e.excerpt().has_value());
  /* And the report is then the message alone, with no caret under a
   * line that is not there. */
  CHECK(e.report().find('^') == std::string::npos);
}

ZU_TEST(the_class_of_a_code_is_the_type_it_is_thrown_as) {
  /* The two characters that open a code are the condition class, and
   * one catch per class is the whole reason for the hierarchy. A caller
   * catching DataError catches every condition in class 22, including
   * the ones added after their program was written. */
  auto conn = zu::Connection::memory();

  CHECK_THROWS_AS(zu::SyntaxError, conn.query("MATCH (p:Person RETURN p"));
  CHECK_THROWS_AS(zu::SyntaxError, conn.query("RETURN nope"));
  CHECK_THROWS_AS(zu::DataError, conn.query("RETURN 1 / 0"));
  CHECK_THROWS_AS(zu::DataError, conn.query("RETURN 1 + 'ada'"));

  /* And each of those is reachable by the class of its code rather than
   * by the name of the type, which is what makes the mapping a mapping
   * rather than a list somebody keeps in step by hand. */
  const zu::Error syntax = REFUSAL(conn, "RETURN nope");
  CHECK_EQ(syntax.code()->substr(0, 2), std::string_view("42"));
  const zu::Error data = REFUSAL(conn, "RETURN 1 / 0");
  CHECK_EQ(data.code()->substr(0, 2), std::string_view("22"));
}

ZU_TEST(an_error_carries_its_fields_out_of_the_catch_that_saw_them) {
  /* Everything above is read inside a catch. The strings are the
   * Error's own copies, so all of it still reads once the zu_error is
   * freed and the connection is gone, which is what lets a host put a
   * failure on a queue. */
  zu::Error kept;
  {
    auto conn = zu::Connection::memory();
    kept = REFUSAL(conn, "MATCH (p:Person RETURN p");
  }
  CHECK_EQ(*kept.code(), std::string_view("42001"));
  CHECK_EQ(*kept.doc_url(), std::string_view("https://zu.dev/docs/errors/42001"));
  CHECK(kept.condition().has_value());
  CHECK(kept.position().has_value());
  CHECK(!kept.message().empty());
  CHECK_EQ(*kept.graph(), std::string_view("home"));
}

ZU_TEST(a_failure_that_is_not_a_statement_carries_no_condition_yet) {
  /* This is the gap, written down rather than skipped.
   *
   * Everything above is a statement the engine refused, and those carry
   * the whole of it. A connection that could not be opened and an
   * appender aimed at a table nothing declares carry a status and a
   * sentence and nothing else: no code, no standard text, no doc URL.
   * The scorecard item says every failure, so this client does not hold
   * that item honestly until the engine attaches conditions to the
   * failures that happen before or beside a statement.
   *
   * Asserted rather than left alone so that the day it changes, this
   * fails and somebody comes and reads this comment. */
  zt::TempDir dir("nocondition");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  /* A file that exists and is not a database. The suite writes one
   * rather than borrowing something from the machine, because a case
   * that reads a path outside its own directory is a case that fails on
   * somebody else's box. */
  const std::string notadb = dir.file("notadb.zu");
  zt::write_file(notadb, "this is not a database");
  try {
    zu::Connection::open(notadb);
    zt::fail(__FILE__, __LINE__, "a file that is not a database opened");
  } catch (const zu::Exception& e) {
    CHECK(!e.error().message().empty());
    CHECK(!e.error().code().has_value());
    CHECK(!e.error().doc_url().has_value());
  }

  auto conn = zu::Connection::open(path);
  try {
    conn.appender("Nobody");
    zt::fail(__FILE__, __LINE__, "an appender opened on a table nothing declares");
  } catch (const zu::Exception& e) {
    CHECK(!e.error().message().empty());
    CHECK(!e.error().code().has_value());
    CHECK(!e.error().doc_url().has_value());
  }
}

ZU_TEST_MAIN()

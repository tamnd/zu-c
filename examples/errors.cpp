/* What a failure looks like, and what you can do with one.
 *
 * Every failure is an exception named after what went wrong, carrying
 * the GQLSTATUS the engine gave, the position in the statement when
 * there was one, and a report ready to print. Catch the specific type
 * when you mean to handle one thing, and zu::Exception when you mean
 * all of them. */
#include <zu.hpp>

#include <cstdint>
#include <iostream>

int main() {
  auto conn = zu::Connection::memory();

  /* A statement the parser refuses is a SyntaxError, and the report is
   * the excerpt with a caret under the offending character, which is
   * what a REPL prints and what a log wants. */
  try {
    conn.query("RETURN RETURN");
  } catch (const zu::SyntaxError& e) {
    std::cout << "syntax: " << e.code().value_or("?") << '\n' << e.error().report() << '\n';
  }

  /* Asking for a column the statement did not return is a mistake in
   * the program rather than in the data, so it is a ProgrammingError
   * and it never reaches the engine. */
  auto r = conn.query("RETURN 1 AS one");
  try {
    r.column("two");
  } catch (const zu::ProgrammingError& e) {
    std::cout << "programming: " << e.what() << '\n';
  }

  /* So is reading a value into a type it does not fit. A silently
   * truncated 100000 is the bug this exists to not have. */
  auto big = conn.query("RETURN 100000 AS big");
  try {
    big.row(0).get<std::int16_t>("big");
  } catch (const zu::ProgrammingError& e) {
    std::cout << "programming: " << e.what() << '\n';
  }

  /* zu::Exception catches every one of them and derives from
   * std::runtime_error, so a host that already has a catch-all keeps
   * it. retryable() is the one to branch on: it says whether running
   * the same thing again has any chance of a different answer. */
  try {
    conn.query("MATCH (");
  } catch (const zu::Exception& e) {
    std::cout << "any: " << e.code().value_or("?") << ", retryable " << std::boolalpha
              << e.retryable() << '\n';
  }

#if ZU_HAS_EXPECTED
  /* And the other half of the same model, for a build with exceptions
   * off or a caller who would rather branch than catch. Same errors,
   * same codes, returned instead of thrown. */
  const auto bad = conn.try_query("RETURN RETURN");
  if (!bad.has_value()) {
    std::cout << "expected: " << bad.error().code().value_or("?") << '\n';
  }
#endif
}

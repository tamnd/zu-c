/* The error model without exceptions.
 *
 * Every fallible call has a try_ spelling that answers
 * std::expected<T, zu::Error> instead of throwing. It is the same code
 * underneath, so the two agree on everything except how they leave: the
 * same GQLSTATUS, the same message, the same position. Reach for it in
 * a build with exceptions off, on a hot path where a throw is a cost
 * you would rather not have, or because you prefer and_then to catch.
 *
 * It needs C++23. Under C++20 the header keeps the throwing half and
 * says so through ZU_HAS_EXPECTED, and this file compiles to a line
 * saying as much. */
#include <zu.hpp>

#include <cstdint>
#include <iostream>

#if ZU_HAS_EXPECTED

int main() {
  auto conn = zu::Connection::try_memory();
  if (!conn.has_value()) {
    std::cerr << "no connection: " << conn.error().message() << '\n';
    return 1;
  }

  /* A chain that reads a value out of a result, where any step failing
   * is the whole thing failing and there is no place to write an if. */
  const auto answer =
      conn->try_query("RETURN 41 AS v")
          .and_then([](zu::Result r) -> zu::expected<std::int64_t> {
            return r.try_cell(0, 0).and_then([](zu::Value v) { return v.try_as_int(); });
          })
          .transform([](std::int64_t v) { return v + 1; });
  std::cout << "the answer is " << answer.value_or(-1) << '\n';

  /* And the same chain over a statement that does not parse, which
   * short circuits at the first step and carries the error out. */
  const auto bad = conn->try_query("RETURN RETURN").transform([](zu::Result r) {
    return r.rows();
  });
  if (!bad.has_value()) {
    std::cout << "refused: " << bad.error().code().value_or("?") << ", "
              << bad.error().message() << '\n';
  }

  /* A call that returns nothing answers expected<void>, so the check is
   * the same shape whether there was a value to carry or not. */
  auto stmt = conn->try_prepare("RETURN $v AS v");
  if (stmt.has_value() && stmt->try_bind("v", std::int64_t{7}).has_value()) {
    const auto r = stmt->try_execute();
    std::cout << "bound and ran: " << r->row(0).get<std::int64_t>(0) << '\n';
  }
  return 0;
}

#else

int main() {
  std::cout << "this toolchain has no std::expected, so the throwing half is the whole "
               "API here\n";
  auto conn = zu::Connection::memory();
  std::cout << conn.query("RETURN 1 AS one").rows() << " row\n";
}

#endif /* ZU_HAS_EXPECTED */

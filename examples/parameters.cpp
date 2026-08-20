/* Preparing once and running many times.
 *
 * A prepared statement keeps its plan and its bindings, so a loop
 * rebinds only what changed and pays for parsing once. Bind by name,
 * never by pasting a value into the text: a parameter is the only
 * spelling that cannot be talked into meaning something else. */
#include <zu.hpp>

#include <cstdint>
#include <iostream>
#include <optional>

int main() {
  auto conn = zu::Connection::memory();

  auto stmt = conn.prepare("RETURN $v AS v");
  for (std::int64_t v : {1, 2, 3}) {
    auto r = stmt.bind("v", v).execute();
    std::cout << "v = " << r.row(0).get<std::int64_t>("v") << '\n';
  }

  /* Every scalar the ABI carries, and each one keeps its type on the
   * way through rather than becoming a string and back. */
  auto mixed = conn.prepare("RETURN $i AS i, $f AS f, $s AS s, $b AS b, $n AS n");
  auto r = mixed.bind("i", std::int64_t{7})
               .bind("f", 1.5)
               .bind("s", "ada")
               .bind("b", true)
               .bind("n", nullptr)
               .execute();

  auto row = r.row(0);
  std::cout << "i = " << row.get<std::int64_t>("i") << '\n'
            << "f = " << row.get<double>("f") << '\n'
            << "s = " << row.get<std::string_view>("s") << '\n'
            << "b = " << std::boolalpha << row.get<bool>("b") << '\n';

  /* A null read into an optional is empty rather than an exception,
   * which is the difference between a value that is missing and a read
   * that was wrong. */
  const auto n = row.get<std::optional<std::int64_t>>("n");
  std::cout << "n = " << (n.has_value() ? "something" : "null") << '\n';

  /* An optional binds the same way round: engaged is the value, empty
   * is a null. */
  std::optional<std::int64_t> maybe;
  std::cout << "empty optional binds as "
            << (stmt.bind("v", maybe).execute().row(0).is_null(0) ? "null" : "a value")
            << '\n';
}

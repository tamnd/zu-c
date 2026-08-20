/* The same thing in C++, which is most of the argument for the wrapper.
 *
 * No status codes, no out-parameters, no free calls, no goto. A query
 * is a range of rows and a row is indexed by the name the statement
 * gave the column. What fails throws, and what throws is named after
 * what went wrong. */
#include <zu.hpp>

#include <iostream>

int main() {
  auto conn = zu::Connection::memory();

  auto rows = conn.query("UNWIND ['ada', 'grace', 'alan'] AS name RETURN name");
  std::cout << rows.rows() << " rows over " << rows.cols() << " column\n";

  /* The string_view points into the result's bytes, which is why rows
   * is a named variable rather than a temporary: a view outlives
   * nothing, and the result is what it borrows from. */
  for (auto row : rows) {
    std::cout << "  " << row.get<std::string_view>("name") << '\n';
  }
}

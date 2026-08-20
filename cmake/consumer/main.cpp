/* The smallest program that proves the package works: it found the
 * header, it found the engine behind it, and it ran a statement. */
#include <zu.hpp>

#include <cstdint>
#include <iostream>

int main() {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS one");
  if (r.rows() != 1 || r.row(0).get<std::int64_t>("one") != 1) {
    std::cerr << "the engine answered something else\n";
    return 1;
  }
  std::cout << "zu " << zu::version() << ", ABI " << zu::abi_version() << '\n';
}

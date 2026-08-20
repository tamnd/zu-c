/* Transactions, where C++ needs less machinery than most languages.
 *
 * No block taking a lambda and no `with`, because a destructor is the
 * block. A guard that leaves its scope without a commit rolls back,
 * and it does not care whether the scope ended by returning, by
 * breaking or by throwing. */
#include <zu.hpp>

#include <iostream>
#include <stdexcept>

int main() {
  auto conn = zu::Connection::memory();

  {
    auto tx = conn.transaction();
    conn.query("UNWIND [1, 2, 3] AS v RETURN v");
    tx.commit();
  }
  std::cout << std::boolalpha << "in a transaction after commit: " << conn.in_transaction()
            << '\n';

  /* Leaving without a commit rolls back, which is the case worth
   * writing down because it is the one nobody remembers to write. */
  {
    auto tx = conn.transaction();
    std::cout << "inside the guard: " << conn.in_transaction() << '\n';
  }
  std::cout << "after leaving the scope: " << conn.in_transaction() << '\n';

  /* And a throw is just another way of leaving. */
  try {
    auto tx = conn.transaction();
    throw std::runtime_error("something went wrong halfway through");
  } catch (const std::runtime_error& e) {
    std::cout << "caught: " << e.what() << '\n';
  }
  std::cout << "after the throw: " << conn.in_transaction() << '\n';

  /* A read-only transaction is the one to take around a report, so a
   * write cannot slip in under a query that is meant to be reading one
   * consistent thing. */
  {
    auto tx = conn.transaction(true);
    std::cout << conn.query("RETURN 1 AS one").rows() << " row read under a read-only tx\n";
    tx.commit();
  }

  /* The three bare calls are there for a host putting a scope of its
   * own around them. transaction() is the one to reach for otherwise,
   * and it is [[nodiscard]] because a guard nobody kept is a
   * transaction that ends on the next line. */
  conn.begin();
  conn.rollback();
  std::cout << "after the bare calls: " << conn.in_transaction() << '\n';
}

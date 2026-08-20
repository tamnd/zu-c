/* Beginning, committing, rolling back, and the scope guard that does
 * the last one for you.
 *
 * C++ does not need a transaction block taking a lambda, because a
 * destructor is the block: a Transaction that goes out of scope without
 * a commit rolls back, whether the scope ended by returning or by
 * throwing. */
#include <zu.hpp>

#include <stdexcept>
#include <string>

#include "fixture.hpp"
#include "harness.hpp"

ZU_TEST(a_connection_knows_whether_it_is_in_one) {
  auto conn = zu::Connection::memory();
  CHECK(!conn.in_transaction());
  {
    auto tx = conn.transaction();
    CHECK(conn.in_transaction());
    CHECK(static_cast<bool>(tx));
    tx.commit();
    CHECK(!static_cast<bool>(tx));
  }
  CHECK(!conn.in_transaction());
}

ZU_TEST(a_rollback_ends_it_too) {
  auto conn = zu::Connection::memory();
  auto tx = conn.transaction();
  tx.rollback();
  CHECK(!conn.in_transaction());
}

ZU_TEST(a_transaction_that_goes_out_of_scope_rolls_back) {
  auto conn = zu::Connection::memory();
  {
    auto tx = conn.transaction();
    CHECK(conn.in_transaction());
  }
  CHECK(!conn.in_transaction());
}

ZU_TEST(a_transaction_that_is_thrown_out_of_rolls_back) {
  auto conn = zu::Connection::memory();
  bool caught = false;
  try {
    auto tx = conn.transaction();
    CHECK(conn.in_transaction());
    throw std::runtime_error("no");
  } catch (const std::runtime_error& e) {
    caught = true;
    CHECK_EQ(std::string(e.what()), std::string("no"));
  }
  CHECK(caught);
  /* The throw is the caller's own and reaches the caller, and the
   * transaction is gone all the same. */
  CHECK(!conn.in_transaction());
}

ZU_TEST(a_read_only_transaction_is_still_a_transaction) {
  auto conn = zu::Connection::memory();
  auto tx = conn.transaction(true);
  CHECK(conn.in_transaction());
  CHECK_EQ(conn.query("RETURN 1 AS one").rows(), 1u);
  tx.commit();
  CHECK(!conn.in_transaction());
}

ZU_TEST(a_statement_runs_inside_an_open_transaction) {
  auto conn = zu::Connection::memory();
  auto tx = conn.transaction();
  CHECK_EQ(conn.query("UNWIND [1, 2] AS v RETURN v").rows(), 2u);
  tx.commit();
}

ZU_TEST(the_bare_calls_are_there_for_a_caller_who_wants_them) {
  /* A host wrapping this in its own object needs the three calls
   * without the guard, and they are the same three underneath. */
  auto conn = zu::Connection::memory();
  conn.begin();
  CHECK(conn.in_transaction());
  conn.commit();
  CHECK(!conn.in_transaction());
  conn.begin();
  conn.rollback();
  CHECK(!conn.in_transaction());
}

ZU_TEST(committing_twice_is_the_callers_mistake_and_not_a_crash) {
  auto conn = zu::Connection::memory();
  auto tx = conn.transaction();
  tx.commit();
  /* The guard is spent, so a second commit is a no-op rather than a
   * commit of whatever transaction came next. */
  tx.commit();
  CHECK(!conn.in_transaction());
  CHECK_THROWS_AS(zu::Exception, conn.commit());
}

ZU_TEST(two_connections_on_one_database_have_transactions_of_their_own) {
  auto db = zu::Database::memory();
  auto first = db.connect();
  auto second = db.connect();
  auto tx = first.transaction();
  CHECK(first.in_transaction());
  CHECK(!second.in_transaction());
  tx.rollback();
}

ZU_TEST(a_transaction_moves_and_the_one_it_left_does_nothing) {
  auto conn = zu::Connection::memory();
  auto tx = conn.transaction();
  auto moved = std::move(tx);
  CHECK(!static_cast<bool>(tx));
  CHECK(static_cast<bool>(moved));
  CHECK(conn.in_transaction());
  moved.commit();
  CHECK(!conn.in_transaction());
}

ZU_TEST(a_query_inside_a_transaction_over_a_file_reads_what_was_loaded) {
  zt::TempDir dir("tx");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto tx = conn.transaction(true);
  CHECK_EQ(conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int(), 3);
  tx.commit();
}

ZU_TEST_MAIN()

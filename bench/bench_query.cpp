/* What one statement costs.
 *
 * The number that matters for an embedded database is not throughput on
 * a million rows, which is a scan, but the floor under a single small
 * statement, because that is what an application does ten thousand
 * times a second. There is no socket and no serialization here, so the
 * floor is parse, plan, run and hand back.
 *
 * Read the first two rows together. Preparing is supposed to buy the
 * parse and the plan, and on this engine it does not, because the
 * connection caches plans and a repeated one-shot query hits that cache
 * anyway. Preparing is still the right spelling when there are
 * parameters, since that is the only safe way to put a value in a
 * statement, but it is not a speed argument here and this file is where
 * that would show up if it ever became one. */
#include <zu.hpp>

#include <cstdint>

#include "harness.hpp"

int main() {
  auto conn = zu::Connection::memory();

  zb::run("query, one row, one column", 1, [&] {
    auto r = conn.query("RETURN 1 AS one");
    zb::keep(r.row(0).get<std::int64_t>(0));
  });

  auto stmt = conn.prepare("RETURN 1 AS one");
  zb::run("prepared, one row, one column", 1, [&] {
    auto r = stmt.execute();
    zb::keep(r.row(0).get<std::int64_t>(0));
  });

  auto param = conn.prepare("RETURN $v AS v");
  std::int64_t n = 0;
  zb::run("prepared with one parameter", 1, [&] {
    auto r = param.bind("v", n++).execute();
    zb::keep(r.row(0).get<std::int64_t>(0));
  });

  zb::run("query, five columns of mixed type", 1, [&] {
    auto r = conn.query("RETURN 1 AS i, 1.5 AS f, 'ada' AS s, true AS b, null AS n");
    zb::keep(r.row(0).get<std::int64_t>(0));
  });

  zb::run("preparing a statement", 1, [&] {
    auto s = conn.prepare("RETURN $v AS v");
    zb::keep(s.raw());
  });

  /* Beginning and ending a transaction, which an application that
   * wraps every unit of work in one pays for on every unit of work. */
  zb::run("begin and commit", 1, [&] {
    auto tx = conn.transaction();
    tx.commit();
  });

  zb::print("one statement at a time");
}

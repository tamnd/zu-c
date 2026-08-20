/* The zero copy path, measured against the alternative.
 *
 * A frame names buffers the host already has as a table. Nothing is
 * copied, nothing is converted, and the only thing that crosses is a
 * pointer, so registering a million rows should cost about the same as
 * registering a thousand. That is the claim, and the first two rows
 * below are it: if registration scaled with the row count, something
 * is copying.
 *
 * The rest is what a query over borrowed memory costs, so that the
 * saving on the way in is not quietly spent on the way through. */
#include <zu.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "harness.hpp"

namespace {

std::vector<std::int64_t> ids(std::size_t n) {
  std::vector<std::int64_t> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::int64_t>(i);
  }
  return out;
}

}  // namespace

int main() {
  const std::vector<std::int64_t> thousand = ids(1'000);
  const std::vector<std::int64_t> million = ids(1'000'000);

  auto conn = zu::Connection::memory();

  zb::run("register a frame of 1k rows", 1, [&] {
    auto f = zu::Frame::create("Small", thousand.size());
    f.column("id", thousand);
    conn.register_frame(f);
    conn.unregister_frame("Small");
  });

  zb::run("register a frame of 1m rows", 1, [&] {
    auto f = zu::Frame::create("Big", million.size());
    f.column("id", million);
    conn.register_frame(f);
    conn.unregister_frame("Big");
  });

  /* Several columns at once, because a frame with five of them is the
   * shape a dataframe arrives in. */
  const std::vector<double> scores(1'000'000, 1.5);
  zb::run("register five columns of 1m rows", 1, [&] {
    auto f = zu::Frame::create("Wide", million.size());
    f.column("a", million)
        .column("b", million)
        .column("c", scores)
        .column("d", scores)
        .column("e", million);
    conn.register_frame(f);
    conn.unregister_frame("Wide");
  });

  zb::print("registering borrowed memory");

  auto big = zu::Frame::create("Person", million.size());
  big.column("id", million);
  conn.register_frame(big);

  zb::run("scan 1m borrowed rows and count", 1'000'000, [&] {
    zb::keep(conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int());
  });

  zb::run("scan 1m borrowed rows with a predicate", 1'000'000, [&] {
    zb::keep(conn.query("MATCH (p:Person) WHERE p.id < 100 RETURN count(*) AS n")
                 .cell(0, 0)
                 .as_int());
  });

  zb::run("scan 1m borrowed rows into a column", 1'000'000, [&] {
    auto r = conn.query("MATCH (p:Person) RETURN p.id AS id");
    zb::keep(r.ints(0).size());
  });

  zb::print("querying borrowed memory");
}

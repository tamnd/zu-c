/* Three ways of reading the same million rows.
 *
 * The whole argument for the columnar accessors is here in one table.
 * ints() hands back a span over the engine's own buffer, so summing it
 * is a loop over an array and the optimizer can see all of it. The
 * row-at-a-time spellings go through a bounds check and a type tag per
 * cell, which is the price of being able to read a column whose type
 * you do not know until runtime.
 *
 * Both are useful. The point of measuring them together is that the
 * gap should be a decision a caller makes on purpose rather than a
 * surprise they find in production. */
#include <zu.hpp>

#include <cstdint>
#include <numeric>
#include <vector>

#include "harness.hpp"

namespace {

constexpr std::size_t ROWS = 1'000'000;

std::vector<std::int64_t> ids() {
  std::vector<std::int64_t> out(ROWS);
  for (std::size_t i = 0; i < ROWS; ++i) {
    out[i] = static_cast<std::int64_t>(i);
  }
  return out;
}

}  // namespace

int main() {
  const std::vector<std::int64_t> buffer = ids();
  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Person", ROWS);
  frame.column("id", buffer);
  conn.register_frame(frame);

  /* One result, read over and over, so what is measured is the read and
   * not the statement that produced it. */
  auto r = conn.query("MATCH (p:Person) RETURN p.id AS id");

  zb::run("column as a span, summed", ROWS, [&] {
    const auto v = r.ints("id");
    zb::keep(std::accumulate(v.begin(), v.end(), std::int64_t{0}));
  });

  zb::run("row.get<int64_t>(0), summed", ROWS, [&] {
    std::int64_t total = 0;
    for (auto row : r) {
      total += row.get<std::int64_t>(0);
    }
    zb::keep(total);
  });

  zb::run("cell(row, 0).as_int(), summed", ROWS, [&] {
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < r.rows(); ++i) {
      total += r.cell(i, 0).as_int();
    }
    zb::keep(total);
  });

  /* Looking a column up by name costs a comparison against every name
   * once, which is nothing next to a million rows and is worth knowing
   * about in a loop that reads one row at a time. */
  zb::run("column index by name", 1, [&] { zb::keep(r.column("id")); });

  /* And the same read against a plain std::vector, which is the honest
   * baseline: this is what the numbers above are trying to reach. */
  zb::run("std::vector, summed", ROWS, [&] {
    zb::keep(std::accumulate(buffer.begin(), buffer.end(), std::int64_t{0}));
  });

  zb::print("reading a million rows");

  /* Strings are the interesting case, because a view into the result is
   * free and a std::string is a copy and an allocation per row. */
  auto words = conn.query("UNWIND ['ada', 'grace', 'alan', 'hedy'] AS name RETURN name");
  zb::run("string_view per row", 4, [&] {
    std::size_t total = 0;
    for (auto row : words) {
      total += row.get<std::string_view>(0).size();
    }
    zb::keep(total);
  });
  zb::run("std::string per row", 4, [&] {
    std::size_t total = 0;
    for (auto row : words) {
      total += row.get<std::string>(0).size();
    }
    zb::keep(total);
  });
  zb::print("reading strings");
}

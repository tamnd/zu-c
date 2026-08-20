/* Reading a column instead of a row, which is the reason to embed a
 * database rather than talk to one.
 *
 * A column comes back as a std::span over the engine's own buffer. It
 * is not copied, converted, boxed or reallocated on the way, so summing
 * a million integers is a loop over a million integers and the standard
 * algorithms work on it directly. The span is good until the Result is
 * destroyed and not one line longer. */
#include <zu.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <numeric>

int main() {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [4, 8, 15, 16, 23, 42] AS v RETURN v");

  const std::span<const std::int64_t> v = r.ints("v");
  std::cout << v.size() << " values\n"
            << "sum " << std::accumulate(v.begin(), v.end(), std::int64_t{0}) << '\n'
            << "max " << *std::ranges::max_element(v) << '\n';

  /* Read twice and it is the same pointer, because there was never a
   * copy to hand out a second one of. */
  std::cout << "same buffer both times: " << std::boolalpha
            << (r.ints("v").data() == r.ints(0).data()) << '\n';

  /* Nulls are carried beside the values rather than in them, one byte a
   * row: 1 where the cell holds a value and 0 where it does not. A byte
   * rather than a bit costs seven eighths of nothing and saves a shift
   * and a mask on every row. */
  auto sparse = conn.query("UNWIND [1, null, 3] AS v RETURN v");
  const auto values = sparse.ints("v");
  const auto valid = sparse.valid("v");
  for (std::size_t i = 0; i < values.size(); ++i) {
    std::cout << "  row " << i << ": ";
    if (valid[i] != 0) {
      std::cout << values[i] << '\n';
    } else {
      std::cout << "null\n";
    }
  }

  /* A large result arrives as several chunks, and a host that wants to
   * hand each one to a thread walks them rather than the rows. */
  std::cout << r.chunk_count() << " chunk(s)\n";
  for (std::uint64_t c = 0; c < r.chunk_count(); ++c) {
    const auto extent = r.chunk(c);
    std::cout << "  chunk " << c << ": rows " << extent.offset << " to "
              << extent.offset + extent.rows << '\n';
  }
}

/* Querying memory the host already has, without copying it anywhere.
 *
 * A frame names a std::vector as a table. The buffers stay where they
 * are, the engine reads them where they lie, and the only thing that
 * crosses the boundary is a pointer and a length. This is the direction
 * the loader does not go: no file, no import, no second copy of the
 * data in a format the database prefers.
 *
 * The layouts are Arrow's, so a column that came out of Arrow, polars
 * or numpy is registered as it stands. */
#include <zu.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

int main() {
  auto conn = zu::Connection::memory();

  const std::vector<std::int64_t> ids{1, 2, 3, 4};
  const std::vector<double> scores{0.5, 1.5, 2.5, 3.5};
  /* The width and the signedness come from the type of the buffer, so
   * a column of int16_t is read as one rather than widened first. */
  const std::vector<std::int16_t> ages{31, 47, 25, 60};

  auto people = zu::Frame::create("Person", ids.size());
  people.column("id", ids).column("score", scores).column("age", ages);
  conn.register_frame(people);

  auto r = conn.query(
      "MATCH (p:Person) WHERE p.score > 1.0 RETURN p.id AS id, p.age AS age ORDER BY p.id");
  std::cout << r.rows() << " people over the threshold\n";
  for (auto row : r) {
    std::cout << "  id " << row.get<std::int64_t>("id") << ", age "
              << row.get<std::int64_t>("age") << '\n';
  }

  /* Strings are Arrow's Utf8 layout: the characters end to end, and
   * n + 1 offsets saying where each one starts. Nothing is NUL
   * terminated and nothing needs to be. */
  const std::vector<std::string> words{"ada", "grace", "alan", "hedy"};
  std::vector<std::int32_t> offsets{0};
  std::string chars;
  for (const std::string& w : words) {
    chars += w;
    offsets.push_back(static_cast<std::int32_t>(chars.size()));
  }
  auto named = zu::Frame::create("Named", words.size());
  named.column("id", ids);
  named.strings("name", offsets, std::span<const char>(chars.data(), chars.size()));
  conn.register_frame(named);

  auto names = conn.query("MATCH (n:Named) RETURN n.name AS name ORDER BY n.id");
  std::cout << "names:";
  for (auto row : names) {
    std::cout << ' ' << row.get<std::string_view>("name");
  }
  std::cout << '\n';

  /* Buffers the host does not intend to keep alive by hand go in with a
   * keepalive, and the engine drops its share after the last reader
   * rather than at the unregister. */
  auto owned = std::make_shared<std::vector<std::int64_t>>(std::vector<std::int64_t>{7, 8});
  auto held = zu::Frame::create("Held", owned->size(), owned);
  held.column("id", *owned);
  conn.register_frame(held);
  std::cout << conn.query("MATCH (h:Held) RETURN count(*) AS n").cell(0, 0).as_int()
            << " rows in the shared buffer\n";

  for (const std::string& name : conn.registered()) {
    std::cout << "registered: " << name << '\n';
  }
}

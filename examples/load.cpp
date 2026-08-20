/* Building a database out of columns you already have.
 *
 * The engine has no CREATE TABLE worth the name, so a bulk load is how
 * a graph comes into being: name the two tables, say how many rows,
 * hand over one array per property, and add the edges as the row each
 * one starts at and the row it ends at. Nothing reaches the file until
 * finish, so a load either happened or did not. */
#include <zu.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "zu-example-load.zu";
  std::filesystem::remove(path);

  const std::vector<std::int64_t> ids{1, 2, 3, 4};
  const std::vector<std::string> names{"ada", "grace", "alan", "hedy"};
  /* Edges by row offset rather than by key, because at load time the
   * rows are the only names there are. */
  const std::vector<std::uint32_t> from{0, 1, 2};
  const std::vector<std::uint32_t> to{1, 2, 3};

  {
    auto loader = zu::Loader::create(path.string());
    loader.table("Person", "Knows", ids.size());
    loader.ints("id", ids).strings("name", names).edges(from, to).finish();
  }

  auto conn = zu::Connection::open(path.string());

  auto people = conn.query("MATCH (p:Person) RETURN p.name AS name ORDER BY p.id");
  std::cout << people.rows() << " people\n";
  for (auto row : people) {
    std::cout << "  " << row.get<std::string_view>("name") << '\n';
  }

  auto edges = conn.query(
      "MATCH (a:Person)-[:Knows]->(b:Person) RETURN a.name AS src, b.name AS dst "
      "ORDER BY a.id");
  std::cout << edges.rows() << " edges\n";
  for (auto row : edges) {
    std::cout << "  " << row.get<std::string_view>("src") << " knows "
              << row.get<std::string_view>("dst") << '\n';
  }

  /* Rows into a table that already exists go through the appender
   * instead, which buffers and flushes rather than building a file. */
  {
    auto rows = conn.appender("Person");
    rows.row(std::int64_t{5}, "katherine");
    std::cout << rows.close() << " row appended\n";
  }
  std::cout << conn.query("MATCH (p:Person) RETURN count(*) AS n").cell(0, 0).as_int()
            << " people now\n";

  std::filesystem::remove(path);
}

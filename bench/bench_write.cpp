/* Getting rows in, which is where an embedded database is usually
 * either won or lost.
 *
 * Two paths, and they are for different things. The loader builds a
 * database out of arrays the caller already has, so the cost per row is
 * a memcpy and a write. The appender adds rows to a table that already
 * exists, one at a time, so the cost per row includes the buffering and
 * the flush.
 *
 * Each measured call here builds a whole database in a temporary
 * directory and deletes it afterwards, so what is measured includes the
 * file system. That is the honest number: a load that is not on disk at
 * the end of it did not happen. */
#include <zu.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "harness.hpp"

namespace {

constexpr std::size_t ROWS = 100'000;

std::vector<std::int64_t> ids(std::size_t n) {
  std::vector<std::int64_t> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i] = static_cast<std::int64_t>(i);
  }
  return out;
}

std::vector<std::string> names(std::size_t n) {
  std::vector<std::string> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back("person-" + std::to_string(i));
  }
  return out;
}

/* A directory of its own, so a run that is interrupted leaves nothing
 * behind in anybody's temp. */
class Scratch {
 public:
  Scratch() : dir_(std::filesystem::temp_directory_path() / "zu-bench-write") {
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  ~Scratch() { std::filesystem::remove_all(dir_); }

  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;

  std::string next() {
    return (dir_ / ("db-" + std::to_string(n_++) + ".zu")).string();
  }
  void drop(const std::string& path) { std::filesystem::remove(path); }

 private:
  std::filesystem::path dir_;
  std::uint64_t n_ = 0;
};

}  // namespace

int main() {
  Scratch scratch;
  const std::vector<std::int64_t> id_column = ids(ROWS);
  const std::vector<std::string> name_column = names(ROWS);

  zb::run("loader, 100k rows, one integer column", ROWS, [&] {
    const std::string path = scratch.next();
    {
      auto loader = zu::Loader::create(path);
      loader.table("Person", "Knows", ROWS);
      loader.ints("id", id_column).finish();
    }
    scratch.drop(path);
  });

  zb::run("loader, 100k rows, integer and string", ROWS, [&] {
    const std::string path = scratch.next();
    {
      auto loader = zu::Loader::create(path);
      loader.table("Person", "Knows", ROWS);
      loader.ints("id", id_column).strings("name", name_column).finish();
    }
    scratch.drop(path);
  });

  /* Edges are sorted and deduplicated at finish, so they are worth a
   * row of their own rather than being folded into the number above. */
  std::vector<std::uint32_t> from(ROWS - 1);
  std::vector<std::uint32_t> to(ROWS - 1);
  for (std::size_t i = 0; i + 1 < ROWS; ++i) {
    from[i] = static_cast<std::uint32_t>(i);
    to[i] = static_cast<std::uint32_t>(i + 1);
  }
  zb::run("loader, 100k rows and 100k edges", ROWS, [&] {
    const std::string path = scratch.next();
    {
      auto loader = zu::Loader::create(path);
      loader.table("Person", "Knows", ROWS);
      loader.ints("id", id_column).edges(from, to).finish();
    }
    scratch.drop(path);
  });

  zb::print("building a database");

  /* The appender against a database that is already there. A smaller
   * count, because this one is per row rather than per column and the
   * point is the cost of a row rather than the cost of a file. */
  constexpr std::size_t APPEND = 10'000;
  const std::string base = scratch.next();
  {
    auto loader = zu::Loader::create(base);
    loader.table("Person", "Knows", 1);
    const std::vector<std::int64_t> one{0};
    const std::vector<std::string> one_name{"seed"};
    loader.ints("id", one).strings("name", one_name).finish();
  }

  auto conn = zu::Connection::open(base);
  zb::run("appender, 10k rows of two columns", APPEND, [&] {
    auto rows = conn.appender("Person");
    for (std::size_t i = 0; i < APPEND; ++i) {
      rows.row(id_column[i], name_column[i]);
    }
    zb::keep(rows.close());
  });

  zb::run("appender, 10k rows, discarded", APPEND, [&] {
    auto rows = conn.appender("Person");
    for (std::size_t i = 0; i < APPEND; ++i) {
      rows.row(id_column[i], name_column[i]);
    }
    zb::keep(rows.discard());
  });

  zb::print("appending to a database");
}

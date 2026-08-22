/* The one database several files need.
 *
 * The engine has no DDL worth the name, so a bulk load is how a table
 * comes into being and every case that wants rows to query builds them
 * first. That is three lines, and it is three lines in enough files to
 * be worth writing once.
 */
#ifndef ZU_TEST_FIXTURE_HPP
#define ZU_TEST_FIXTURE_HPP

#include <zu.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace zt {

/* Bytes at a path, for the cases that want a file the engine will
 * refuse. Written here rather than borrowed from the machine, because a
 * case that reads a path outside its own directory is a case that fails
 * on somebody else's box. */
inline void write_file(const std::string& path, std::string_view contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/* A date as the count of days the engine keeps one in, worked out by
 * <chrono> rather than written down, because a number nobody can read
 * is a number nobody can check. */
inline std::int64_t days(int year, unsigned month, unsigned day) {
  const std::chrono::year_month_day ymd{std::chrono::year{year}, std::chrono::month{month},
                                        std::chrono::day{day}};
  return std::chrono::sys_days{ymd}.time_since_epoch().count();
}

/* Three people at the path given, with an id, a name and a Knows edge
 * table that starts empty. */
inline void people(const std::string& path) {
  auto loader = zu::Loader::create(path);
  loader.table("Person", "Knows", 3);
  const std::array<std::int64_t, 3> ids{1, 2, 3};
  const std::array<std::string_view, 3> names{"ada", "grace", "alan"};
  loader.ints("id", ids).strings("name", names).finish();
}

/* Every column the loader takes, so that a case about reading can pick
 * the one it wants. Born is a date, as a count of days from the epoch. */
inline void everything(const std::string& path) {
  auto loader = zu::Loader::create(path);
  loader.table("Thing", "Near", 2);
  const std::array<std::int64_t, 2> n{7, 8};
  const std::array<double, 2> d{1.5, 2.5};
  const std::array<std::int32_t, 2> ok{1, 0};
  const std::vector<std::string> s{"one", "two"};
  const std::array<std::int64_t, 2> born{days(1815, 12, 10), days(1906, 12, 9)};
  loader.ints("n", n)
      .doubles("d", d)
      .bools("ok", ok)
      .strings("s", s)
      .temporals("born", zu::TemporalKind::date, born)
      .finish();
}

}  // namespace zt

#endif /* ZU_TEST_FIXTURE_HPP */

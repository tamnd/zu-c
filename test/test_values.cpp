/* Reading a cell that is not a number.
 *
 * Lists, records, nodes, edges and the seven temporals, all of which
 * come back through zu::Value because a column of them has no flat
 * layout to hand out a span over. */
#include <zu.hpp>

#include <chrono>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

ZU_TEST(a_cell_says_what_it_is) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS i, 1.5 AS f, 'ada' AS s, true AS b, null AS n");
  CHECK_EQ(r.type(0, 0), zu::Type::integer);
  CHECK_EQ(r.type(0, 1), zu::Type::floating);
  CHECK_EQ(r.type(0, 2), zu::Type::string);
  CHECK_EQ(r.type(0, 3), zu::Type::boolean);
  CHECK_EQ(r.type(0, 4), zu::Type::null);
  CHECK(r.cell(0, 4).is_null());
}

ZU_TEST(a_list_is_a_range_of_values) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN ['ada', 'grace'] AS v");
  const zu::Value list = r.cell(0, 0);
  CHECK_EQ(list.type(), zu::Type::list);
  CHECK_EQ(list.size(), 2u);
  CHECK(!list.empty());
  CHECK_EQ(list.at(0).as_string(), "ada");
  CHECK_EQ(list[1].as_string(), "grace");

  std::vector<std::string> read;
  for (const zu::Value v : list.elements()) {
    read.emplace_back(v.as_string());
  }
  CHECK_EQ(read.size(), 2u);
  CHECK_EQ(read[0], std::string("ada"));
  CHECK_EQ(read[1], std::string("grace"));
}

ZU_TEST(a_list_of_lists_nests) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN [[1, 2], [3]] AS v");
  const zu::Value outer = r.cell(0, 0);
  CHECK_EQ(outer.size(), 2u);
  CHECK_EQ(outer[0].size(), 2u);
  CHECK_EQ(outer[1].size(), 1u);
  CHECK_EQ(outer[0][1].as_int(), 2);
  CHECK_EQ(outer[1][0].as_int(), 3);
}

ZU_TEST(a_list_index_past_the_end_is_refused) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN [1, 2] AS v");
  const zu::Value list = r.cell(0, 0);
  CHECK_THROWS_AS(zu::Exception, list.at(2));
}

ZU_TEST(a_record_has_names_as_well_as_values) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN {a: 1, b: 'x'} AS v");
  const zu::Value rec = r.cell(0, 0);
  CHECK_EQ(rec.type(), zu::Type::record);
  CHECK_EQ(rec.size(), 2u);
  CHECK_EQ(rec.field(0), "a");
  CHECK_EQ(rec.field(1), "b");
  CHECK_EQ(rec[0].as_int(), 1);
  CHECK_EQ(rec[1].as_string(), "x");
}

ZU_TEST(a_byte_string_comes_back_as_the_octets_it_is) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN X'00AB' AS b");
  CHECK_EQ(r.type(0, 0), zu::Type::bytes);

  const std::span<const std::uint8_t> b = r.cell(0, 0).as_bytes();
  CHECK_EQ(b.size(), 2u);
  /* The leading zero is the point. Read as text this value ends before
   * it starts, which is why the engine has a second type for it and why
   * this reads as a span of octets rather than as a string. */
  CHECK_EQ(static_cast<int>(b[0]), 0);
  CHECK_EQ(static_cast<int>(b[1]), 0xAB);

  const auto same = r.row(0).get<std::span<const std::uint8_t>>(0);
  CHECK_EQ(same.size(), 2u);
  CHECK_EQ(same.data(), b.data());

  /* The copying spelling, for a caller who wants the octets to outlive
   * the result they came out of. */
  const auto owned = r.row(0).get<std::vector<std::uint8_t>>(0);
  CHECK_EQ(owned.size(), 2u);
  CHECK_EQ(static_cast<int>(owned[1]), 0xAB);
}

ZU_TEST(an_empty_byte_string_is_a_byte_string_and_not_a_null) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN X'' AS b");
  CHECK_EQ(r.type(0, 0), zu::Type::bytes);
  CHECK(!r.cell(0, 0).is_null());
  const auto b = r.cell(0, 0).as_bytes();
  CHECK(b.empty());
  /* Empty and iterable, rather than empty and undefined to walk. */
  CHECK_EQ(std::distance(b.begin(), b.end()), 0);
}

ZU_TEST(octets_and_text_are_not_read_as_one_another) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN X'00AB' AS b, 'ada' AS s");
  /* A blob that happened to be valid UTF-8 would read as text and one
   * that did not would be a silent mess, so neither direction is
   * allowed and a caller who guessed wrong is told. */
  CHECK_THROWS_AS(zu::Exception, r.cell(0, 0).as_string());
  CHECK_THROWS_AS(zu::Exception, r.cell(0, 1).as_bytes());
}

ZU_TEST(a_node_is_a_table_and_a_row) {
  zt::TempDir dir("node");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto r = conn.query("MATCH (p:Person) RETURN p ORDER BY p.id");
  CHECK_EQ(r.rows(), 3u);
  const zu::Value first = r.cell(0, 0);
  CHECK_EQ(first.type(), zu::Type::node);
  const zu::Node n = first.as_node();
  const zu::Node again = r.row(0).get<zu::Node>(0);
  CHECK_EQ(n.table, again.table);
  CHECK_EQ(n.offset, again.offset);
  /* Three nodes of one table, so the offsets differ and the table does
   * not. */
  CHECK_EQ(r.cell(2, 0).as_node().table, n.table);
  CHECK_NE(r.cell(2, 0).as_node().offset, n.offset);
}

ZU_TEST(a_table_id_says_what_it_is_called) {
  zt::TempDir dir("tablename");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto r = conn.query("MATCH (p:Person) RETURN p ORDER BY p.id");
  const zu::Node n = r.cell(0, 0).as_node();

  const std::optional<std::string> name = conn.table_name(n.table);
  CHECK(name.has_value());
  CHECK_EQ(*name, std::string("Person"));

  /* Nothing rather than a failure, because an id no table has is an
   * answer to the question and not a broken call. */
  CHECK(!conn.table_name(9999).has_value());

  /* A copy, so asking again does not move the first answer out from
   * under it. The pointer the ABI hands back would have. */
  const std::optional<std::string> again = conn.table_name(n.table);
  CHECK_EQ(*name, *again);
}

ZU_TEST(a_node_column_reads_as_a_span_of_offsets) {
  zt::TempDir dir("nodespan");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto r = conn.query("MATCH (p:Person) RETURN p ORDER BY p.id");
  const auto offsets = r.node_offsets(0);
  CHECK_EQ(offsets.size(), 3u);
  CHECK_EQ(offsets[0], r.cell(0, 0).as_node().offset);
  CHECK_EQ(offsets[2], r.cell(2, 0).as_node().offset);
}

ZU_TEST(a_date_comes_back_as_the_day_it_was_written) {
  zt::TempDir dir("date");
  const std::string path = dir.file("things.zu");
  zt::everything(path);

  auto conn = zu::Connection::open(path);
  auto r = conn.query("MATCH (t:Thing) RETURN t.born AS born ORDER BY t.n");
  CHECK_EQ(r.rows(), 2u);
  const zu::Temporal born = r.row(0).get<zu::Temporal>("born");
  CHECK_EQ(born.kind, zu::TemporalKind::date);
  CHECK_EQ(born.count, zt::days(1815, 12, 10));
  CHECK_EQ(born.offset, 0);

  /* The chrono round trip, which is the point of carrying a kind: a
   * count of days is only a date once something says it is. */
  const std::chrono::year_month_day ymd{born.as_days()};
  CHECK_EQ(static_cast<int>(ymd.year()), 1815);
  CHECK_EQ(static_cast<unsigned>(ymd.month()), 12u);
  CHECK_EQ(static_cast<unsigned>(ymd.day()), 10u);
}

ZU_TEST(the_chrono_constructors_and_readers_agree) {
  using namespace std::chrono;

  const zu::Temporal d = zu::Temporal::date(year{1969} / December / 31);
  CHECK_EQ(d.kind, zu::TemporalKind::date);
  CHECK_EQ(d.count, -1);
  CHECK_EQ(d.as_days().time_since_epoch().count(), -1);

  const zu::Temporal t = zu::Temporal::local_time(hours{13} + minutes{45});
  CHECK_EQ(t.kind, zu::TemporalKind::local_time);
  CHECK_EQ(t.as_nanos(), nanoseconds(hours{13} + minutes{45}));
  CHECK_EQ(t.offset, 0);

  const zu::Temporal z = zu::Temporal::zoned_time(hours{9}, minutes{330});
  CHECK_EQ(z.kind, zu::TemporalKind::zoned_time);
  CHECK_EQ(z.east(), minutes{330});

  const auto midday = sys_days{year{2000} / January / 1} + hours{12};
  const zu::Temporal ldt = zu::Temporal::local_datetime(midday);
  CHECK_EQ(ldt.kind, zu::TemporalKind::local_datetime);
  CHECK_EQ(ldt.as_time(), midday);

  const zu::Temporal zdt = zu::Temporal::zoned_datetime(midday, minutes{-480});
  CHECK_EQ(zdt.kind, zu::TemporalKind::zoned_datetime);
  CHECK_EQ(zdt.as_time(), midday);
  CHECK_EQ(zdt.east(), minutes{-480});

  const zu::Temporal ym = zu::Temporal::months(std::chrono::months{18});
  CHECK_EQ(ym.kind, zu::TemporalKind::duration_year_month);
  CHECK_EQ(ym.as_months(), std::chrono::months{18});

  const zu::Temporal dt = zu::Temporal::nanos(seconds{90});
  CHECK_EQ(dt.kind, zu::TemporalKind::duration_day_time);
  CHECK_EQ(dt.as_nanos(), nanoseconds(seconds{90}));

  /* Value equality is member equality, so a temporal read back from the
   * engine compares against one written here. */
  CHECK(d == zu::Temporal::date(sys_days{days{-1}}));
  CHECK(d != t);
}

ZU_TEST_MAIN()

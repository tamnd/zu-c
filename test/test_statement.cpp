/* Preparing once and running many times, with parameters. */
#include <zu.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

ZU_TEST(a_parameter_crosses_and_comes_back) {
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $v AS v");
  auto r = stmt.bind("v", std::int64_t{42}).execute();
  CHECK_EQ(r.row(0).get<std::int64_t>("v"), 42);
}

ZU_TEST(every_scalar_kind_of_parameter) {
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $v AS v");

  CHECK_EQ(stmt.bind("v", std::int64_t{7}).execute().row(0).get<std::int64_t>(0), 7);
  CHECK_EQ(stmt.bind("v", 1.5).execute().row(0).get<double>(0), 1.5);
  CHECK_EQ(stmt.bind("v", true).execute().row(0).get<bool>(0), true);
  /* Kept rather than read out of the temporary, because a view borrows
   * from the result and a result that died at the semicolon has nothing
   * left to borrow from. */
  auto strings = stmt.bind("v", "ada").execute();
  CHECK_EQ(strings.row(0).get<std::string_view>(0), "ada");
  CHECK(stmt.bind_null("v").execute().row(0).is_null(0));
  CHECK(stmt.bind("v", nullptr).execute().row(0).is_null(0));
}

ZU_TEST(an_int_binds_without_being_written_as_an_int64) {
  /* A caller who writes 42 rather than std::int64_t{42} means the same
   * thing and should not have to say so. */
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $v AS v");
  CHECK_EQ(stmt.bind("v", 42).execute().row(0).get<std::int64_t>(0), 42);
}

ZU_TEST(an_optional_binds_as_the_value_or_as_null) {
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $v AS v");

  const std::optional<std::int64_t> some{5};
  const std::optional<std::int64_t> none;
  CHECK_EQ(stmt.bind("v", some).execute().row(0).get<std::int64_t>(0), 5);
  CHECK(stmt.bind("v", none).execute().row(0).is_null(0));
}

ZU_TEST(bindings_chain_and_a_statement_runs_again) {
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $a AS a, $b AS b");
  auto r = stmt.bind("a", std::int64_t{1}).bind("b", "two").execute();
  CHECK_EQ(r.row(0).get<std::int64_t>("a"), 1);
  CHECK_EQ(r.row(0).get<std::string_view>("b"), "two");

  /* Nothing was rebound, so the same values answer again. */
  auto again = stmt.execute();
  CHECK_EQ(again.row(0).get<std::int64_t>("a"), 1);
}

ZU_TEST(a_prepared_statement_runs_in_a_loop) {
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $v AS v");
  std::vector<std::int64_t> seen;
  for (std::int64_t v : {1, 2, 3}) {
    seen.push_back(stmt.bind("v", v).execute().row(0).get<std::int64_t>(0));
  }
  CHECK_EQ(seen.size(), 3u);
  CHECK_EQ(seen[0], 1);
  CHECK_EQ(seen[2], 3);
}

ZU_TEST(a_temporal_goes_out_and_comes_back) {
  using namespace std::chrono;
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $v AS v");

  const zu::Temporal date = zu::Temporal::date(year{2026} / August / 20);
  const zu::Temporal read = stmt.bind("v", date).execute().row(0).get<zu::Temporal>(0);
  CHECK_EQ(read.kind, zu::TemporalKind::date);
  CHECK_EQ(read.count, date.count);

  const zu::Temporal time = zu::Temporal::local_time(hours{13} + minutes{45});
  const zu::Temporal time_back = stmt.bind("v", time).execute().row(0).get<zu::Temporal>(0);
  CHECK_EQ(time_back.kind, zu::TemporalKind::local_time);
  CHECK_EQ(time_back.count, time.count);

  const zu::Temporal zoned = zu::Temporal::zoned_time(hours{9} + minutes{30}, minutes{120});
  const zu::Temporal zoned_back = stmt.bind("v", zoned).execute().row(0).get<zu::Temporal>(0);
  CHECK_EQ(zoned_back.kind, zu::TemporalKind::zoned_time);
  CHECK_EQ(zoned_back.offset, 120);

  const zu::Temporal ym = zu::Temporal::months(std::chrono::months{14});
  const zu::Temporal ym_back = stmt.bind("v", ym).execute().row(0).get<zu::Temporal>(0);
  CHECK_EQ(ym_back.kind, zu::TemporalKind::duration_year_month);
  CHECK_EQ(ym_back.count, 14);

  const zu::Temporal dur = zu::Temporal::nanos(hours{25});
  const zu::Temporal dur_back = stmt.bind("v", dur).execute().row(0).get<zu::Temporal>(0);
  CHECK_EQ(dur_back.kind, zu::TemporalKind::duration_day_time);
  CHECK_EQ(dur_back.count, nanoseconds(hours{25}).count());
}

ZU_TEST(a_temporal_read_out_of_one_result_binds_into_the_next) {
  using namespace std::chrono;
  auto conn = zu::Connection::memory();
  auto stmt = conn.prepare("RETURN $v AS v");

  zu::Temporal out;
  {
    auto r = stmt.bind("v", zu::Temporal::date(year{1999} / December / 31)).execute();
    out = r.row(0).get<zu::Temporal>(0);
  }
  const zu::Temporal back = stmt.bind("v", out).execute().row(0).get<zu::Temporal>(0);
  CHECK(back == out);
}

ZU_TEST(a_statement_that_does_not_parse_is_refused_at_prepare) {
  auto conn = zu::Connection::memory();
  CHECK_THROWS_AS(zu::SyntaxError, conn.prepare("MATCH ("));
}

ZU_TEST(a_result_outlives_the_statement_that_made_it) {
  auto conn = zu::Connection::memory();
  zu::Result r;
  {
    auto stmt = conn.prepare("RETURN $v AS v");
    r = stmt.bind("v", std::int64_t{7}).execute();
  }
  CHECK_EQ(r.row(0).get<std::int64_t>(0), 7);
}

ZU_TEST(a_prepared_statement_over_a_table_reads_it) {
  zt::TempDir dir("stmt");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto stmt = conn.prepare("MATCH (p:Person) RETURN p.id AS id, p.name AS name ORDER BY p.id");
  auto r = stmt.execute();
  CHECK_EQ(r.rows(), 3u);
  CHECK_EQ(r.row(1).get<std::string_view>("name"), "grace");
}

ZU_TEST_MAIN()

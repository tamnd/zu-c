/* Reading a column rather than a row.
 *
 * This is the part of the API that is the reason to embed a database
 * rather than talk to one: an integer column comes back as a span over
 * the engine's own buffer, and nothing is copied, converted or boxed on
 * the way. A test that checks the numbers is checking the easy half.
 * The half worth checking is that the pointer is the engine's. */
#include <zu.hpp>

#include <numeric>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

ZU_TEST(an_integer_column_is_a_span) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2, 3, 4] AS v RETURN v");
  const std::span<const std::int64_t> v = r.ints(0);
  CHECK_EQ(v.size(), 4u);
  CHECK_EQ(v[0], 1);
  CHECK_EQ(v[3], 4);
  CHECK_EQ(std::accumulate(v.begin(), v.end(), std::int64_t{0}), 10);
}

ZU_TEST(a_column_read_twice_is_the_same_buffer) {
  /* If this ever copies, this is the case that says so. */
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2, 3] AS v RETURN v");
  CHECK_EQ(r.ints(0).data(), r.ints(0).data());
}

ZU_TEST(a_column_is_read_by_name_as_well) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2, 3] AS v RETURN v");
  CHECK_EQ(r.ints("v").size(), 3u);
  CHECK_EQ(r.ints("v").data(), r.ints(0).data());
}

ZU_TEST(a_float_column_is_a_span_of_doubles) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1.5, 2.5] AS v RETURN v");
  const auto v = r.doubles(0);
  CHECK_EQ(v.size(), 2u);
  CHECK_EQ(v[0], 1.5);
  CHECK_EQ(v[1], 2.5);
}

ZU_TEST(an_integer_column_widens_to_doubles_and_a_string_does_not) {
  /* col_f64 reads floats and integers, which is the one widening the
   * engine does for free. A column of characters is not a column of
   * numbers by any reading, and asking for it as one is refused. */
  auto conn = zu::Connection::memory();
  auto ints = conn.query("UNWIND [1, 2] AS v RETURN v");
  CHECK_EQ(ints.doubles(0)[1], 2.0);

  auto strings = conn.query("UNWIND ['ada'] AS v RETURN v");
  CHECK_THROWS_AS(zu::Exception, strings.doubles(0));
  CHECK_THROWS_AS(zu::Exception, strings.ints(0));
}

ZU_TEST(a_validity_bitmap_says_which_rows_are_there) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, null, 3] AS v RETURN v");
  /* One byte a row rather than one bit, which costs three bytes here
   * and saves a shift and a mask on every row of a million. */
  const auto valid = r.valid(0);
  CHECK_EQ(valid.size(), 3u);
  CHECK(valid[0] != 0);
  CHECK(valid[1] == 0);
  CHECK(valid[2] != 0);

  /* And it agrees with the row-at-a-time answer, which is the property
   * that matters: two ways of asking, one truth. */
  for (std::uint64_t i = 0; i < r.rows(); ++i) {
    CHECK_EQ(valid[i] != 0, !r.row(i).is_null(0));
  }
}

ZU_TEST(a_column_with_no_nulls_still_answers_a_bitmap) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2, 3] AS v RETURN v");
  const auto valid = r.valid(0);
  CHECK_EQ(valid.size(), r.rows());
  for (std::uint64_t i = 0; i < r.rows(); ++i) {
    CHECK(valid[i] != 0);
  }
}

ZU_TEST(a_span_of_a_column_and_the_rows_agree) {
  zt::TempDir dir("cols");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  auto r = conn.query("MATCH (p:Person) RETURN p.id AS id ORDER BY p.id");
  const auto ids = r.ints("id");
  CHECK_EQ(ids.size(), r.rows());
  for (std::uint64_t i = 0; i < r.rows(); ++i) {
    CHECK_EQ(ids[i], r.row(i).get<std::int64_t>("id"));
  }
}

ZU_TEST(every_column_of_a_loaded_table_reads_as_itself) {
  zt::TempDir dir("everything");
  const std::string path = dir.file("things.zu");
  zt::everything(path);

  auto conn = zu::Connection::open(path);
  auto r = conn.query(
      "MATCH (t:Thing) RETURN t.n AS n, t.d AS d, t.ok AS ok, t.s AS s ORDER BY t.n");
  CHECK_EQ(r.rows(), 2u);
  CHECK_EQ(r.ints("n")[0], 7);
  CHECK_EQ(r.ints("n")[1], 8);
  CHECK_EQ(r.doubles("d")[0], 1.5);
  CHECK_EQ(r.row(0).get<bool>("ok"), true);
  CHECK_EQ(r.row(1).get<bool>("ok"), false);
  CHECK_EQ(r.str(0, r.column("s")), "one");
  CHECK_EQ(r.str(1, r.column("s")), "two");
}

ZU_TEST(a_result_is_one_chunk_or_several_and_the_chunks_cover_it) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2, 3, 4, 5] AS v RETURN v");
  CHECK(r.chunk_count() >= 1u);

  std::uint64_t covered = 0;
  std::vector<std::int64_t> read;
  for (std::uint64_t c = 0; c < r.chunk_count(); ++c) {
    const auto extent = r.chunk(c);
    CHECK_EQ(extent.offset, covered);
    covered += extent.rows;
    const auto v = r.chunk_ints(c, 0);
    CHECK_EQ(v.size(), extent.rows);
    read.insert(read.end(), v.begin(), v.end());
  }
  CHECK_EQ(covered, r.rows());
  CHECK_EQ(read.size(), 5u);
  CHECK_EQ(read[0], 1);
  CHECK_EQ(read[4], 5);
}

ZU_TEST(a_chunk_that_is_not_there_is_refused) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2] AS v RETURN v");
  CHECK_THROWS_AS(zu::Exception, r.chunk(r.chunk_count()));
  CHECK_THROWS_AS(zu::Exception, r.chunk_ints(r.chunk_count(), 0));
}

ZU_TEST(a_chunk_has_a_validity_bitmap_of_its_own) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, null, 3] AS v RETURN v");
  for (std::uint64_t c = 0; c < r.chunk_count(); ++c) {
    const auto extent = r.chunk(c);
    const auto valid = r.chunk_valid(c, 0);
    CHECK_EQ(valid.size(), extent.rows);
    for (std::uint64_t i = 0; i < extent.rows; ++i) {
      CHECK_EQ(valid[i] != 0, !r.row(extent.offset + i).is_null(0));
    }
  }
}

ZU_TEST(a_result_with_no_rows_has_empty_columns_rather_than_none) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [] AS v RETURN v");
  CHECK(r.ints(0).empty());
  CHECK(r.valid(0).empty());
}

ZU_TEST_MAIN()

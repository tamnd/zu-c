/* The shapes the language expects, pinned rather than described.
 *
 * Every other file here checks what the wrapper answers. This one
 * checks what it *is*: that a Result is a range the standard views
 * compose over, that a handle moves and refuses to be copied, that an
 * exception is a std::exception, that the value structs are regular,
 * and that a view into a result cannot silently outlive it.
 *
 * Most of it is static_assert, which is the point. A concept that holds
 * is a promise the compiler keeps at every call site rather than one a
 * case checked once; and when a change breaks it, it breaks here, with
 * the name of the concept in the message, instead of three repositories
 * away inside a std::views expression a user wrote.
 *
 * The runtime half is there so the static half cannot be satisfied by
 * something that compiles and does nothing. A concept says a pipeline
 * would compile; the case below runs one and checks the number.
 *
 * One promise of this item is not testable from inside the language.
 * Ninety three calls here are [[nodiscard]], and the failure they catch
 * is a caller writing `conn.try_query(q);` and dropping the expected
 * that IS the failure report. A case that dropped one to prove the
 * warning fires would be a case that fails the build, since CI compiles
 * this repository with -Werror. So that one is enforced at every call
 * site by the compiler and is deliberately not a case here.
 */
#include <zu.hpp>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <exception>
#include <iterator>
#include <numeric>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

/* ---- a result is a range ---- */

static_assert(std::ranges::range<zu::Result>);
static_assert(std::ranges::sized_range<zu::Result>);
static_assert(std::ranges::common_range<zu::Result>);
/* Random access rather than input, which is the difference between
 * `rows[500]` costing a bounds check and costing five hundred reads.
 * The rows are all in memory before the first is handed out, so an
 * input range here would be the wrapper lying downwards. */
static_assert(std::ranges::random_access_range<zu::Result>);
static_assert(std::random_access_iterator<std::ranges::iterator_t<zu::Result>>);
static_assert(std::same_as<std::ranges::range_value_t<zu::Result>, zu::Row>);

/* And NOT a borrowed range, which is the compiler's half of the rule
 * the README states in English: a Row points into the Result it came
 * from, so an iterator that outlived the Result would point into freed
 * memory.
 *
 * Because this is false, `conn.query(q) | std::views::filter(f)` over a
 * temporary answers a std::ranges::dangling rather than an iterator,
 * and the mistake is a compile error at the point it is written. A
 * Result marked borrowed would turn that into a use after free that
 * happens to work in a debug build. */
static_assert(!std::ranges::borrowed_range<zu::Result>);

static_assert(std::ranges::view<zu::ValueRange>);
static_assert(std::ranges::random_access_range<zu::ValueRange>);
static_assert(std::same_as<std::ranges::range_value_t<zu::ValueRange>, zu::Value>);

/* ---- a handle is moved, never copied ---- */

/* A copied connection would be two owners of one zu_conn and a double
 * close, so the copy is gone rather than deep. Movable, because a
 * factory that answers one has to be able to. */
static_assert(std::movable<zu::Connection>);
static_assert(!std::copyable<zu::Connection>);
static_assert(std::is_nothrow_move_constructible_v<zu::Connection>);
static_assert(std::movable<zu::Result>);
static_assert(!std::copyable<zu::Result>);
static_assert(std::is_nothrow_move_constructible_v<zu::Result>);
static_assert(std::movable<zu::Statement>);
static_assert(!std::copyable<zu::Statement>);
static_assert(std::movable<zu::Frame>);
static_assert(!std::copyable<zu::Frame>);

/* A transaction moves and does not copy, on the same reasoning: it is a
 * scope guard, and two guards over one transaction would roll back
 * twice. Moving is allowed because a host that wants to hand the guard
 * to the scope below it has to be able to. */
static_assert(std::movable<zu::Transaction>);
static_assert(!std::copyable<zu::Transaction>);

/* Nothing here needs a destructor called by hand, which is what makes
 * the throwing spelling safe. */
static_assert(std::is_nothrow_destructible_v<zu::Connection>);
static_assert(std::is_nothrow_destructible_v<zu::Result>);
static_assert(std::is_nothrow_destructible_v<zu::Transaction>);

/* ---- a failure is an exception ---- */

static_assert(std::derived_from<zu::Exception, std::runtime_error>);
static_assert(std::derived_from<zu::Exception, std::exception>);
static_assert(std::derived_from<zu::SyntaxError, zu::Exception>);
static_assert(std::derived_from<zu::DataError, zu::Exception>);
static_assert(std::derived_from<zu::ConnectionError, zu::Exception>);
static_assert(std::derived_from<zu::TransactionError, zu::Exception>);
static_assert(std::derived_from<zu::ProgrammingError, zu::Exception>);
static_assert(std::derived_from<zu::ConcurrentError, zu::Exception>);
static_assert(std::derived_from<zu::ClosedError, zu::Exception>);
static_assert(std::derived_from<zu::InterruptedError, zu::Exception>);
static_assert(std::derived_from<zu::InternalError, zu::Exception>);

/* An Error is a value and travels like one, which is what lets a
 * std::expected carry it and a host store it. */
static_assert(std::copyable<zu::Error>);
static_assert(std::is_nothrow_move_constructible_v<zu::Error>);

/* ---- the value structs are regular ---- */

/* Default constructible, copyable, comparable: what the standard
 * library means by a value. A struct that was not could not go in a
 * std::vector and be searched for, which is the first thing a caller
 * does with a node id. */
static_assert(std::regular<zu::Node>);
static_assert(std::regular<zu::Rel>);
static_assert(std::regular<zu::Position>);
static_assert(std::regular<zu::Temporal>);
static_assert(std::is_trivially_copyable_v<zu::Node>);
static_assert(std::is_trivially_copyable_v<zu::Rel>);

/* ---- and the cases ---- */

ZU_TEST(a_result_composes_with_the_standard_views) {
  zt::TempDir dir("idiom-views");
  const std::string path = dir.file("people.zu1");
  zt::people(path);
  auto conn = zu::Connection::open(path);

  auto rows = conn.query("MATCH (p:Person) RETURN p.id AS id, p.name AS name ORDER BY p.id");

  /* The pipeline a caller would actually write. If any of the concepts
   * above stopped holding, this is the expression that would stop
   * compiling. */
  std::vector<std::string> loud;
  for (auto name : rows | std::views::filter([](zu::Row r) { return r.get<std::int64_t>("id") != 2; }) |
                       std::views::transform([](zu::Row r) { return r.get<std::string>("name"); })) {
    loud.push_back(std::move(name));
  }
  CHECK_EQ(loud.size(), 2u);
  CHECK_EQ(loud[0], std::string("ada"));
  CHECK_EQ(loud[1], std::string("alan"));

  /* And the algorithms, which take the range rather than two
   * iterators, because that is how they are written in C++20. Of ada,
   * grace and alan, one is three letters. */
  const auto short_names = std::ranges::count_if(
      rows, [](zu::Row r) { return r.get<std::string_view>("name").size() == 3; });
  CHECK_EQ(short_names, 1);

  /* Random access, used as random access. */
  CHECK_EQ(rows.begin()[2].get<std::string_view>("name"), std::string_view("alan"));
  CHECK_EQ(std::ranges::distance(rows), 3);
}

ZU_TEST(a_connection_moves_and_the_one_it_left_is_still_safe_to_destroy) {
  auto first = zu::Connection::memory();
  CHECK(static_cast<bool>(first));

  auto second = std::move(first);
  /* The moved-from handle is empty rather than undefined, which is what
   * lets its destructor run at the end of this scope without a second
   * close of the connection `second` now owns. */
  CHECK(!static_cast<bool>(first));
  CHECK(static_cast<bool>(second));
  CHECK_EQ(second.query("RETURN 7 AS v").cell(0, 0).as_int(), 7);
}

ZU_TEST(a_failure_is_caught_by_the_class_of_its_condition_and_by_std_exception) {
  auto conn = zu::Connection::memory();

  /* By its own class. */
  CHECK_THROWS_AS(zu::SyntaxError, conn.query("RETURN RETURN"));

  /* By the base, which is what a host with one catch at the top of a
   * request writes. */
  bool caught = false;
  try {
    conn.query("RETURN RETURN");
  } catch (const std::exception& e) {
    caught = true;
    /* what() is the message and not a class name, because the string a
     * generic handler logs should be the one that says what happened. */
    CHECK(std::string_view(e.what()).size() > 0);
    CHECK(std::string_view(e.what()) != std::string_view("zu::Exception"));
  }
  CHECK(caught);

  /* And the whole report is still on the exception, which is the half a
   * return code cannot carry.
   *
   * An unclosed bracket rather than RETURN RETURN, because the second
   * one parses: RETURN is accepted as a variable name, so the failure
   * comes from the binder at no particular token and carries no
   * position. That is tamnd/zu#579's neighbourhood and not this case's
   * subject. */
  try {
    conn.query("MATCH (p:Person RETURN p");
  } catch (const zu::Exception& e) {
    CHECK(e.error().position().has_value());
    CHECK(e.error().code().has_value());
    CHECK(e.error().excerpt().has_value());
  }
}

ZU_TEST(the_value_structs_go_in_containers_and_come_back_out) {
  std::vector<zu::Node> nodes{{0, 3}, {0, 1}, {1, 1}};
  CHECK(std::ranges::find(nodes, zu::Node{1, 1}) != nodes.end());
  CHECK(std::ranges::find(nodes, zu::Node{9, 9}) == nodes.end());

  /* Sorted by a comparator rather than by <=>, because these carry no
   * ordering: two tables number their rows from zero and there is no
   * one order the pair is in. */
  std::ranges::sort(nodes, [](zu::Node a, zu::Node b) {
    return a.table != b.table ? a.table < b.table : a.offset < b.offset;
  });
  CHECK_EQ(nodes.front(), (zu::Node{0, 1}));
}

/* ---- printing ---- */

ZU_TEST(every_kind_of_thing_prints_as_something_a_person_can_read) {
  CHECK_EQ(zu::to_string(zu::Status::misuse_concurrent),
           std::string_view("misuse_concurrent"));
  CHECK_EQ(zu::to_string(zu::Severity::warning), std::string_view("warning"));
  CHECK_EQ(zu::to_string(zu::Type::integer), std::string_view("int"));
  CHECK_EQ(zu::to_string(zu::TemporalKind::duration_day_time),
           std::string_view("duration_day_time"));

  CHECK_EQ(zu::to_string(zu::Position{3, 14, 40}), std::string("line 3, column 14"));
  CHECK_EQ(zu::to_string(zu::Node{2, 7}), std::string("node 2:7"));
  CHECK_EQ(zu::to_string(zu::Rel{1, 4, 9}), std::string("rel 1:4->9"));

  /* The kind first, because 19000 is a date in 2022 and a duration of
   * nineteen microseconds and nothing else tells them apart. */
  CHECK_EQ(zu::to_string(zu::Temporal{zu::TemporalKind::date, 19000, 0}),
           std::string("date 19000"));
  CHECK_EQ(zu::to_string(zu::Temporal{zu::TemporalKind::zoned_time, 5, 90}),
           std::string("zoned_time 5 +90"));
  CHECK_EQ(zu::to_string(zu::Temporal{zu::TemporalKind::zoned_time, 5, -90}),
           std::string("zoned_time 5 -90"));

  /* A status the ABI has and this header has not is a mystery rather
   * than a walk off the end of a switch. */
  CHECK_EQ(zu::to_string(static_cast<zu::Status>(9999)), std::string_view("unknown"));
}

ZU_TEST(a_cell_prints_as_what_it_holds) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 1 AS i, 1.5 AS f, 'ada' AS s, true AS b, null AS n");

  CHECK_EQ(zu::to_string(r.cell(0, 0)), std::string("1"));
  CHECK_EQ(zu::to_string(r.cell(0, 1)), std::string("1.5"));
  CHECK_EQ(zu::to_string(r.cell(0, 2)), std::string("ada"));
  CHECK_EQ(zu::to_string(r.cell(0, 3)), std::string("true"));
  CHECK_EQ(zu::to_string(r.cell(0, 4)), std::string("null"));

  /* A cell nobody read is not a crash. */
  CHECK_EQ(zu::to_string(zu::Value{}), std::string("<none>"));

  /* Fifteen significant digits, so a tenth prints as a tenth. This is
   * the printing path and not the round trip: a caller who needs the
   * bits has as_double. */
  auto tenth = conn.query("RETURN 0.1 AS f");
  CHECK_EQ(zu::to_string(tenth.cell(0, 0)), std::string("0.1"));

  /* The shapes a scalar cannot hold say what they are and how many they
   * hold, because a list printed elementwise is a different function
   * and the caller who wants it has elements(). */
  auto list = conn.query("RETURN ['ada', 'grace'] AS v");
  CHECK_EQ(zu::to_string(list.cell(0, 0)), std::string("list of 2"));

  auto bytes = conn.query("RETURN X'00AB' AS b");
  CHECK_EQ(zu::to_string(bytes.cell(0, 0)), std::string("2 bytes"));
}

ZU_TEST(a_failure_prints_on_one_line_with_its_code_and_its_place_in_front) {
  auto conn = zu::Connection::memory();
  try {
    conn.query("MATCH (p:Person RETURN p");
  } catch (const zu::Exception& e) {
    const std::string line = zu::to_string(e.error());
    /* Everything on one line, which is what a log wants; report() is
     * the other spelling, with a caret under the column. */
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line.find(std::string(*e.error().code())) == 0);
    CHECK(line.find("line 1, column 17") != std::string::npos);
    CHECK(line.find(std::string(e.error().message())) != std::string::npos);
    CHECK(zu::to_string(e.error()) != e.error().report());
  }

  /* A condition with no code and no position is the message alone, and
   * not an empty prefix and a colon. */
  const zu::Error bare = zu::Error::take(zu::Status::interrupted, nullptr);
  CHECK(!bare.code().has_value());
  CHECK_EQ(zu::to_string(bare), std::string(bare.message()));
}

#if ZU_HAS_FORMAT
ZU_TEST(std_format_prints_the_same_text_and_takes_the_whole_format_spec) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("RETURN 42 AS v");

  /* One implementation under both spellings, so they cannot come to
   * disagree about what a value looks like. */
  CHECK_EQ(std::format("{}", zu::Status::conflict),
           std::string(zu::to_string(zu::Status::conflict)));
  CHECK_EQ(std::format("{}", zu::Node{2, 7}), zu::to_string(zu::Node{2, 7}));
  CHECK_EQ(std::format("{}", r.cell(0, 0)), std::string("42"));

  /* The spec comes with the base class rather than being written here,
   * which is the reason for inheriting formatter<string_view> instead
   * of implementing parse. */
  CHECK_EQ(std::format("[{:>8}]", zu::Type::string), std::string("[     str]"));
  CHECK_EQ(std::format("[{:<6}]", zu::Status::io), std::string("[io    ]"));

  try {
    conn.query("RETURN RETURN");
  } catch (const zu::Exception& e) {
    CHECK_EQ(std::format("{}", e.error()), zu::to_string(e.error()));
  }

  /* And it composes, which is the thing a formatter is for: a row
   * printed by a host is one call and no stream. */
  auto people = conn.query("RETURN 1 AS id, 'ada' AS name");
  CHECK_EQ(std::format("{} is {}", people.cell(0, 0), people.cell(0, 1)),
           std::string("1 is ada"));
}
#endif

#if ZU_HAS_EXPECTED
/* The other half of the error model, and the reason it is std::expected
 * rather than a pair or an optional: it composes. */
ZU_TEST(the_expected_half_composes_the_way_std_expected_is_meant_to) {
  auto conn = zu::Connection::memory();

  const auto answer = conn.try_query("RETURN 41 AS v")
                          .transform([](zu::Result r) { return r.cell(0, 0).as_int() + 1; })
                          .value_or(-1);
  CHECK_EQ(answer, 42);

  /* A failure short circuits the rest of the chain without a branch
   * being written for it, which is the whole point. */
  int ran = 0;
  const auto refused = conn.try_query("RETURN RETURN")
                           .transform([&](zu::Result r) {
                             ++ran;
                             return r.rows();
                           })
                           .value_or(0);
  CHECK_EQ(refused, 0u);
  CHECK_EQ(ran, 0);

  /* And the error that came out is the whole Error, not a code. */
  const auto failed = conn.try_query("MATCH (p:Person RETURN p");
  CHECK(!failed.has_value());
  CHECK(failed.error().position().has_value());
  CHECK(failed.error().code().has_value());
  CHECK_EQ(failed.error().status(), zu::Status::error);

  /* or_else is the recovery path, and it gets the same object. */
  const auto recovered =
      conn.try_query("RETURN RETURN")
          .transform([](zu::Result r) { return r.rows(); })
          .or_else([](const zu::Error& e) -> zu::expected<std::uint64_t> {
            CHECK(e.code().has_value());
            return 99u;
          })
          .value_or(0);
  CHECK_EQ(recovered, 99u);
}

static_assert(std::same_as<zu::expected<int>::error_type, zu::Error>);
#endif

ZU_TEST_MAIN()

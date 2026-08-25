/* Querying memory the caller already has.
 *
 * A frame is the zero copy path in the direction the loader does not
 * go: the buffers stay where the host put them, the engine reads them
 * where they lie, and the only thing that crosses is a pointer. What
 * the tests below check is that every layout Arrow hands out is a
 * layout this reads without moving a byte, and that the promise about
 * lifetime holds: the release callback runs after the last reader, not
 * at the unregister that preceded it. */
#include <zu.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

namespace {

/* Arrow's Utf8 layout out of a list of words: n + 1 offsets and the
 * characters end to end. */
struct Chars {
  std::vector<std::int32_t> offsets32;
  std::vector<std::int64_t> offsets64;
  std::vector<char> data;
};

Chars pack(const std::vector<std::string>& words) {
  Chars out;
  out.offsets32.push_back(0);
  out.offsets64.push_back(0);
  for (const std::string& w : words) {
    out.data.insert(out.data.end(), w.begin(), w.end());
    out.offsets32.push_back(static_cast<std::int32_t>(out.data.size()));
    out.offsets64.push_back(static_cast<std::int64_t>(out.data.size()));
  }
  return out;
}

/* Arrow's Utf8View: sixteen bytes a row. Twelve characters or fewer
 * live in the view itself, and anything longer keeps a four byte prefix
 * and points at a buffer. */
std::vector<std::byte> pack_views(const std::vector<std::string>& words) {
  std::vector<std::byte> out(words.size() * 16, std::byte{0});
  std::int32_t offset = 0;
  for (std::size_t i = 0; i < words.size(); ++i) {
    std::byte* view = out.data() + i * 16;
    const std::int32_t len = static_cast<std::int32_t>(words[i].size());
    std::memcpy(view, &len, 4);
    if (len <= 12) {
      std::memcpy(view + 4, words[i].data(), words[i].size());
    } else {
      const std::int32_t buffer = 0;
      std::memcpy(view + 4, words[i].data(), 4);
      std::memcpy(view + 8, &buffer, 4);
      std::memcpy(view + 12, &offset, 4);
    }
    offset += len;
  }
  return out;
}

std::vector<std::string> column_of(zu::Result& r, std::uint32_t col) {
  std::vector<std::string> out;
  for (auto row : r) {
    out.emplace_back(row.get<std::string_view>(col));
  }
  return out;
}

}  // namespace

ZU_TEST(a_column_of_longs_is_queried_where_it_lies) {
  const std::vector<std::int64_t> ids{1, 2, 3};
  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Person", ids.size());
  frame.column("id", ids);
  conn.register_frame(frame);

  auto r = conn.query("MATCH (p:Person) RETURN p.id AS id ORDER BY id");
  CHECK_EQ(r.rows(), 3u);
  CHECK_EQ(r.ints(0)[0], 1);
  CHECK_EQ(r.ints(0)[2], 3);
}

ZU_TEST(every_width_and_sign_comes_back_as_the_number_it_is) {
  /* The width and the signedness come from the type of the buffer,
   * which is the one thing C++ knows here that C did not. */
  const std::vector<std::int64_t> big{1, -2};
  const std::vector<std::int32_t> mid{3, -4};
  const std::vector<std::int16_t> small{5, -6};
  const std::vector<std::int8_t> tiny{7, 8};
  const std::vector<std::uint32_t> unsign{9, 10};

  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Wide", 2);
  frame.column("big", big)
      .column("mid", mid)
      .column("small", small)
      .column("tiny", tiny)
      .column("unsign", unsign);
  conn.register_frame(frame);

  /* Every alias in accent quotes, because several of these words are
   * reserved and which ones is the grammar's business rather than this
   * test's. A property is read by name and needs no quoting; an alias
   * after AS is parsed as a name and does. */
  auto r = conn.query(
      "MATCH (w:Wide) RETURN w.big AS `big`, w.mid AS `mid`, w.small AS `small`, "
      "w.tiny AS `tiny`, w.unsign AS `unsign`");
  CHECK_EQ(r.row(0).get<std::int64_t>("big"), 1);
  CHECK_EQ(r.row(0).get<std::int64_t>("mid"), 3);
  CHECK_EQ(r.row(0).get<std::int64_t>("small"), 5);
  CHECK_EQ(r.row(0).get<std::int64_t>("tiny"), 7);
  CHECK_EQ(r.row(0).get<std::int64_t>("unsign"), 9);
  CHECK_EQ(r.row(1).get<std::int64_t>("big"), -2);
  CHECK_EQ(r.row(1).get<std::int64_t>("mid"), -4);
  CHECK_EQ(r.row(1).get<std::int64_t>("small"), -6);
  CHECK_EQ(r.row(1).get<std::int64_t>("tiny"), 8);
  CHECK_EQ(r.row(1).get<std::int64_t>("unsign"), 10);
}

ZU_TEST(both_widths_of_float_come_back) {
  const std::vector<double> wide{1.5, 2.5};
  const std::vector<float> narrow{0.5f, 0.25f};

  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Reading", 2);
  frame.column("wide", wide).column("narrow", narrow);
  conn.register_frame(frame);

  auto r = conn.query("MATCH (x:Reading) RETURN x.wide AS wide, x.narrow AS narrow");
  CHECK_EQ(r.row(0).get<double>("wide"), 1.5);
  CHECK_EQ(r.row(0).get<double>("narrow"), 0.5);
  CHECK_EQ(r.row(1).get<double>("wide"), 2.5);
  CHECK_EQ(r.row(1).get<double>("narrow"), 0.25);
}

ZU_TEST(a_float_column_with_a_scale_is_refused) {
  const std::vector<double> wide{1.5, 2.5};
  auto frame = zu::Frame::create("Reading", 2);
  CHECK_THROWS_AS(zu::Exception, frame.column("wide", wide, 1000));
  CHECK_THROWS_AS(zu::Exception, frame.column("wide", wide, 1, zu::TemporalKind::date));
}

ZU_TEST(a_bitmap_is_a_column_of_booleans) {
  /* 0b1101: true, false, true, true. */
  const std::array<std::uint8_t, 1> bitmap{0b1101};
  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Flag", 4);
  frame.bools("on", bitmap, 4);
  conn.register_frame(frame);

  /* The alias in accent quotes because ON is a reserved word. The
     property is still named on, and a property is read by name rather
     than parsed as one, which is why only the alias needs them. */
  auto r = conn.query("MATCH (f:Flag) RETURN f.on AS `on`");
  CHECK_EQ(r.rows(), 4u);
  CHECK_EQ(r.row(0).get<bool>(0), true);
  CHECK_EQ(r.row(1).get<bool>(0), false);
  CHECK_EQ(r.row(2).get<bool>(0), true);
  CHECK_EQ(r.row(3).get<bool>(0), true);
}

ZU_TEST(characters_end_to_end_are_a_column_of_strings) {
  const std::vector<std::string> words{"ada", "", "grace", "éàü", std::string(300, 'a')};
  const Chars packed = pack(words);

  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Word", words.size());
  frame.strings("text", std::span<const std::int32_t>(packed.offsets32),
                std::span<const char>(packed.data));
  conn.register_frame(frame);

  auto r = conn.query("MATCH (w:Word) RETURN w.text AS text");
  CHECK_EQ(column_of(r, 0), words);
}

ZU_TEST(sixty_four_bit_offsets_are_the_same_column) {
  const std::vector<std::string> words{"ada", "grace", "alan"};
  const Chars packed = pack(words);

  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Word", words.size());
  frame.strings("text", std::span<const std::int64_t>(packed.offsets64),
                std::span<const char>(packed.data));
  conn.register_frame(frame);

  auto r = conn.query("MATCH (w:Word) RETURN w.text AS text");
  CHECK_EQ(column_of(r, 0), words);
}

ZU_TEST(a_view_column_is_read_without_its_characters_moving) {
  const std::vector<std::string> words{"ada", "a string too long to live in sixteen bytes"};
  const Chars packed = pack(words);
  const std::vector<std::byte> views = pack_views(words);
  const std::array<const void*, 1> buffers{packed.data.data()};
  const std::array<std::size_t, 1> lens{packed.data.size()};

  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Word", words.size());
  frame.views("text", views, buffers, lens, words.size());
  conn.register_frame(frame);

  auto r = conn.query("MATCH (w:Word) RETURN w.text AS text");
  CHECK_EQ(column_of(r, 0), words);
}

ZU_TEST(a_date_goes_in_as_the_days_it_is_and_comes_back_as_a_date) {
  const std::vector<std::int32_t> on{19782, 0};
  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Event", 2);
  frame.column("on", on, 1, zu::TemporalKind::date);
  conn.register_frame(frame);

  /* Accent quotes for the same reason they are up in the bitmap test:
     ON is a reserved word and an alias is a name being written. */
  auto r = conn.query("MATCH (e:Event) RETURN e.on AS `on`");
  const zu::Temporal first = r.row(0).get<zu::Temporal>(0);
  CHECK_EQ(first.kind, zu::TemporalKind::date);
  CHECK_EQ(first.count, 19782);
}

ZU_TEST(arrow_microseconds_are_scaled_to_the_nanoseconds_this_engine_counts_in) {
  const std::vector<std::int64_t> at{1'700'000'000'000'000};
  auto conn = zu::Connection::memory();
  auto frame = zu::Frame::create("Event", 1);
  frame.column("at", at, 1000, zu::TemporalKind::local_datetime);
  conn.register_frame(frame);

  auto r = conn.query("MATCH (e:Event) RETURN e.at AS `at`");
  const zu::Temporal read = r.row(0).get<zu::Temporal>(0);
  CHECK_EQ(read.kind, zu::TemporalKind::local_datetime);
  CHECK_EQ(read.count, 1'700'000'000'000'000'000);
}

ZU_TEST(one_frame_serves_as_many_connections_as_there_are_threads_to_query_from_it) {
  const std::vector<std::int64_t> ids{10, 20};
  auto db = zu::Database::memory();
  auto first = db.connect();
  auto second = db.connect();
  auto frame = zu::Frame::create("Person", 2);
  frame.column("id", ids);
  first.register_frame(frame);
  second.register_frame(frame);

  CHECK_EQ(first.query("MATCH (p:Person) RETURN sum(p.id) AS n").cell(0, 0).as_int(), 30);
  CHECK_EQ(second.query("MATCH (p:Person) RETURN sum(p.id) AS n").cell(0, 0).as_int(), 30);
}

ZU_TEST(the_connection_says_what_is_registered_on_it) {
  const std::vector<std::int64_t> a{1};
  const std::vector<std::int64_t> b{2};
  auto conn = zu::Connection::memory();
  auto people = zu::Frame::create("Person", 1);
  auto places = zu::Frame::create("Place", 1);
  people.column("id", a);
  places.column("id", b);

  CHECK(conn.registered().empty());
  conn.register_frame(people);
  conn.register_frame(places);
  CHECK_EQ(conn.registered().size(), 2u);
  CHECK_EQ(conn.registered()[0], std::string("Person"));
  CHECK_EQ(conn.registered()[1], std::string("Place"));
  CHECK(conn.unregister_frame("Person"));
  CHECK_EQ(conn.registered().size(), 1u);
  CHECK(!conn.unregister_frame("Person"));
}

ZU_TEST(the_release_callback_says_when_the_engine_is_finished) {
  const std::vector<std::int64_t> ids{1, 2};
  bool released = false;
  {
    auto conn = zu::Connection::memory();
    {
      auto frame = zu::Frame::create("Person", 2, [&released] { released = true; });
      frame.column("id", ids);
      conn.register_frame(frame);
      conn.query("MATCH (p:Person) RETURN p.id AS id");
      CHECK(!released);
      conn.unregister_frame("Person");
    }
    CHECK(released);
  }
}

ZU_TEST(a_frame_no_connection_ever_saw_still_lets_go_of_what_it_held) {
  bool released = false;
  {
    const std::vector<std::int64_t> ids{1};
    auto frame = zu::Frame::create("Person", 1, [&released] { released = true; });
    frame.column("id", ids);
  }
  CHECK(released);
}

ZU_TEST(a_shared_pointer_keeps_the_buffers_alive_by_itself) {
  /* The spelling for a host that would rather hand over ownership than
   * write a callback: whatever owns the buffers is dropped when the
   * engine is finished, and not before. */
  auto owned = std::make_shared<std::vector<std::int64_t>>(std::vector<std::int64_t>{1, 2, 3});
  const std::weak_ptr<std::vector<std::int64_t>> watch = owned;
  {
    auto conn = zu::Connection::memory();
    auto frame = zu::Frame::create("Person", 3, std::shared_ptr<void>(owned));
    frame.column("id", *owned);
    conn.register_frame(frame);
    owned.reset();
    CHECK(!watch.expired());
    CHECK_EQ(conn.query("MATCH (p:Person) RETURN sum(p.id) AS n").cell(0, 0).as_int(), 6);
  }
  CHECK(watch.expired());
}

ZU_TEST(a_column_of_the_wrong_length_is_refused_at_that_column) {
  const std::vector<std::int64_t> ids{1, 2};
  auto frame = zu::Frame::create("Person", 3);
  CHECK_THROWS_AS(zu::Exception, frame.column("id", ids));
}

ZU_TEST(a_frame_taking_the_name_of_a_stored_table_is_refused) {
  zt::TempDir dir("shadow");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  const std::vector<std::int64_t> ids{9};
  auto conn = zu::Connection::open(path);
  auto frame = zu::Frame::create("Person", 1);
  frame.column("id", ids);
  CHECK_THROWS_AS(zu::Exception, conn.register_frame(frame));
}

ZU_TEST_MAIN()

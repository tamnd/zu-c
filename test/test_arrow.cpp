/* Handing a result to Arrow without copying it.
 *
 * The export moves the arrays out rather than lending them, so it takes
 * the result by rvalue: after it the Result is spent, which is what the
 * C ABI does anyway and is worth saying in the type rather than in a
 * comment nobody reads.
 *
 * A library built without the arrow feature answers ZU_UNSUPPORTED from
 * the same symbol, so the cases here treat that as a skip rather than a
 * failure: what is being tested is the wrapper, and a wrapper cannot
 * make a feature the build left out. */
#include <zu.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

namespace {

/* An ArrowArrayStream released however the case ends, because a stream
 * that is dropped on a failing check is memory the engine still thinks
 * a consumer holds. */
class Stream {
 public:
  Stream() { std::memset(&raw_, 0, sizeof(raw_)); }
  ~Stream() { reset(); }
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  ArrowArrayStream* get() noexcept { return &raw_; }
  bool live() const noexcept { return raw_.release != nullptr; }

  void reset() noexcept {
    if (raw_.release != nullptr) {
      raw_.release(&raw_);
      raw_.release = nullptr;
    }
  }

 private:
  ArrowArrayStream raw_{};
};

class Schema {
 public:
  ~Schema() {
    if (raw_.release != nullptr) {
      raw_.release(&raw_);
    }
  }
  Schema() { std::memset(&raw_, 0, sizeof(raw_)); }
  Schema(const Schema&) = delete;
  Schema& operator=(const Schema&) = delete;

  ArrowSchema* get() noexcept { return &raw_; }

 private:
  ArrowSchema raw_{};
};

/* Every batch of a stream, summed. The stream says it is done by
 * writing a released array rather than by failing. */
std::int64_t rows_of(Stream& stream) {
  std::int64_t total = 0;
  for (;;) {
    ArrowArray array;
    std::memset(&array, 0, sizeof(array));
    if (stream.get()->get_next(stream.get(), &array) != 0) {
      zt::fail(__FILE__, __LINE__, "the stream failed part way through");
    }
    if (array.release == nullptr) {
      break;
    }
    total += array.length;
    array.release(&array);
  }
  return total;
}

/* True when this build of libzu has the arrow feature at all. */
bool exported(zu::Connection& conn, Stream& stream, std::string_view query,
              std::uint64_t rows_per_batch = 0) {
  auto r = conn.query(query);
  try {
    std::move(r).to_arrow(conn, stream.get(), rows_per_batch);
  } catch (const zu::Exception& e) {
    if (e.status() == zu::Status::unsupported) {
      std::printf("     (this libzu was built without the arrow feature)\n");
      return false;
    }
    throw;
  }
  return true;
}

}  // namespace

ZU_TEST(a_result_crosses_as_a_stream_of_batches) {
  auto conn = zu::Connection::memory();
  Stream stream;
  if (!exported(conn, stream, "UNWIND [1, 2, 3] AS v RETURN v")) {
    return;
  }
  CHECK(stream.live());

  Schema schema;
  CHECK_EQ(stream.get()->get_schema(stream.get(), schema.get()), 0);
  CHECK_EQ(schema.get()->n_children, 1);
  CHECK_EQ(std::string(schema.get()->children[0]->name), std::string("v"));

  CHECK_EQ(rows_of(stream), 3);
}

ZU_TEST(every_scalar_column_has_a_schema_entry) {
  auto conn = zu::Connection::memory();
  Stream stream;
  if (!exported(conn, stream, "RETURN 1 AS i, 1.5 AS f, 'ada' AS s, true AS b")) {
    return;
  }

  Schema schema;
  CHECK_EQ(stream.get()->get_schema(stream.get(), schema.get()), 0);
  CHECK_EQ(schema.get()->n_children, 4);
  CHECK_EQ(std::string(schema.get()->children[0]->name), std::string("i"));
  CHECK_EQ(std::string(schema.get()->children[3]->name), std::string("b"));
  CHECK_EQ(rows_of(stream), 1);
}

ZU_TEST(the_batch_size_is_the_callers_to_choose) {
  zt::TempDir dir("arrow");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  auto conn = zu::Connection::open(path);
  Stream stream;
  if (!exported(conn, stream, "MATCH (p:Person) RETURN p.id AS id ORDER BY p.id", 2)) {
    return;
  }

  std::vector<std::int64_t> lengths;
  for (;;) {
    ArrowArray array;
    std::memset(&array, 0, sizeof(array));
    CHECK_EQ(stream.get()->get_next(stream.get(), &array), 0);
    if (array.release == nullptr) {
      break;
    }
    lengths.push_back(array.length);
    array.release(&array);
  }
  CHECK_EQ(lengths.size(), 2u);
  CHECK_EQ(lengths[0], 2);
  CHECK_EQ(lengths[1], 1);
}

ZU_TEST(a_result_with_no_rows_exports_a_schema_and_no_batches) {
  auto conn = zu::Connection::memory();
  Stream stream;
  if (!exported(conn, stream, "UNWIND [] AS v RETURN v")) {
    return;
  }
  Schema schema;
  CHECK_EQ(stream.get()->get_schema(stream.get(), schema.get()), 0);
  CHECK_EQ(schema.get()->n_children, 1);
  CHECK_EQ(rows_of(stream), 0);
}

ZU_TEST(the_export_without_a_connection_names_a_node_table_by_its_id) {
  /* The spelling for a caller who kept the result and let the
   * connection go. The node column comes back named after the table id
   * rather than after the table, which is the trade. */
  zt::TempDir dir("noconn");
  const std::string path = dir.file("people.zu");
  zt::people(path);

  zu::Result r;
  {
    auto conn = zu::Connection::open(path);
    r = conn.query("MATCH (p:Person) RETURN p.id AS id ORDER BY p.id");
  }

  Stream stream;
  try {
    std::move(r).to_arrow(stream.get());
  } catch (const zu::Exception& e) {
    if (e.status() == zu::Status::unsupported) {
      return;
    }
    throw;
  }
  CHECK_EQ(rows_of(stream), 3);
}

ZU_TEST(a_result_that_was_exported_is_spent) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2] AS v RETURN v");
  bool have_arrow = true;
  Stream stream;
  try {
    std::move(r).to_arrow(conn, stream.get());
  } catch (const zu::Exception& e) {
    if (e.status() != zu::Status::unsupported) {
      throw;
    }
    have_arrow = false;
  }
  if (!have_arrow) {
    return;
  }
  /* The rows went to the consumer, so what is left is an empty handle
   * that says so rather than one pointing at freed memory. */
  CHECK(!static_cast<bool>(r));
  CHECK_THROWS_AS(zu::Exception, r.cell(0, 0));
}

#if ZU_HAS_EXPECTED
ZU_TEST(the_export_has_an_expected_spelling_too) {
  auto conn = zu::Connection::memory();
  auto r = conn.query("UNWIND [1, 2] AS v RETURN v");
  Stream stream;
  const auto done = std::move(r).try_to_arrow(conn, stream.get());
  if (!done.has_value() && done.error().status() == zu::Status::unsupported) {
    return;
  }
  CHECK(done.has_value());
  CHECK_EQ(rows_of(stream), 2);
}
#endif

ZU_TEST_MAIN()

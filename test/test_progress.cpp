/* Watching a statement while it runs, and stopping it.
 *
 * A plain scan of a frame is too fast to be reported on at all, which
 * is a nice problem to have and an awkward one to write a test against.
 * A pair of patterns with a predicate over them is a nested loop the
 * planner cannot fold into a count, and three thousand rows of it is
 * about a third of a second. */
#include <zu.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "fixture.hpp"
#include "harness.hpp"

namespace {

constexpr std::size_t ROWS = 3000;

const char* const SLOW = "MATCH (a:Person), (b:Person) WHERE a.id < b.id RETURN count(*)";

std::vector<std::int64_t> ids() {
  std::vector<std::int64_t> out(ROWS);
  for (std::size_t i = 0; i < ROWS; ++i) {
    out[i] = static_cast<std::int64_t>(i);
  }
  return out;
}

/* A database, a connection and a frame of three thousand people, which
 * every case here wants and none of them changes. */
struct Slow {
  std::vector<std::int64_t> buffer = ids();
  zu::Database db = zu::Database::memory();
  zu::Connection conn = db.connect();
  zu::Frame frame = zu::Frame::create("Person", ROWS);

  Slow() {
    frame.column("id", buffer);
    conn.register_frame(frame);
  }
};

}  // namespace

ZU_TEST(the_watcher_is_told_how_far_the_statement_has_got) {
  Slow slow;
  std::mutex lock;
  std::vector<std::pair<std::uint64_t, std::chrono::milliseconds>> calls;

  slow.conn.on_progress(std::chrono::milliseconds{1},
                        [&](std::uint64_t rows, std::chrono::milliseconds elapsed) {
                          const std::lock_guard<std::mutex> held(lock);
                          calls.emplace_back(rows, elapsed);
                          return true;
                        });
  slow.conn.query(SLOW);
  slow.conn.clear_progress();

  CHECK(!calls.empty());
  std::uint64_t rows = 0;
  std::chrono::milliseconds elapsed{0};
  for (const auto& call : calls) {
    CHECK(call.first >= rows);
    CHECK(call.second >= elapsed);
    rows = call.first;
    elapsed = call.second;
  }
  CHECK(rows > 0);
}

ZU_TEST(the_watcher_runs_on_a_thread_of_the_librarys) {
  Slow slow;
  const std::thread::id asked = std::this_thread::get_id();
  std::atomic<bool> same{false};
  std::atomic<int> calls{0};

  slow.conn.on_progress(std::chrono::milliseconds{1}, [&](std::uint64_t, std::chrono::milliseconds) {
    calls.fetch_add(1);
    if (std::this_thread::get_id() == asked) {
      same.store(true);
    }
    return true;
  });
  slow.conn.query(SLOW);
  slow.conn.clear_progress();

  CHECK(calls.load() > 0);
  CHECK(!same.load());
}

ZU_TEST(a_watcher_that_says_no_stops_the_statement) {
  Slow slow;
  slow.conn.on_progress(std::chrono::milliseconds{1},
                        [](std::uint64_t, std::chrono::milliseconds) { return false; });
  CHECK_THROWS_AS(zu::InterruptedError, slow.conn.query(SLOW));
}

ZU_TEST(the_connection_runs_the_next_statement_normally_after_one_was_stopped) {
  Slow slow;
  slow.conn.on_progress(std::chrono::milliseconds{1},
                        [](std::uint64_t, std::chrono::milliseconds) { return false; });
  CHECK_THROWS_AS(zu::InterruptedError, slow.conn.query(SLOW));
  slow.conn.clear_progress();
  CHECK_EQ(slow.conn.query("RETURN 1 AS v").cell(0, 0).as_int(), 1);
}

ZU_TEST(a_watcher_that_throws_stops_the_statement_rather_than_the_process) {
  /* An exception crossing an extern "C" frame is std::terminate, so the
   * trampoline catches everything and answers as though the watcher had
   * asked for the statement to stop. */
  Slow slow;
  slow.conn.on_progress(std::chrono::milliseconds{1},
                        [](std::uint64_t, std::chrono::milliseconds) -> bool {
                          throw std::runtime_error("no");
                        });
  CHECK_THROWS_AS(zu::InterruptedError, slow.conn.query(SLOW));
}

ZU_TEST(taking_the_arrangement_back_stops_the_calls) {
  Slow slow;
  std::atomic<int> calls{0};
  slow.conn.on_progress(std::chrono::milliseconds{1},
                        [&](std::uint64_t, std::chrono::milliseconds) {
                          calls.fetch_add(1);
                          return true;
                        });
  slow.conn.query(SLOW);
  CHECK(calls.load() > 0);

  slow.conn.clear_progress();
  const int seen = calls.load();
  slow.conn.query(SLOW);
  CHECK_EQ(calls.load(), seen);
}

ZU_TEST(an_arrangement_is_replaced_rather_than_added_to) {
  Slow slow;
  std::atomic<int> first{0};
  std::atomic<int> second{0};
  slow.conn.on_progress(std::chrono::milliseconds{1},
                        [&](std::uint64_t, std::chrono::milliseconds) {
                          first.fetch_add(1);
                          return true;
                        });
  slow.conn.on_progress(std::chrono::milliseconds{1},
                        [&](std::uint64_t, std::chrono::milliseconds) {
                          second.fetch_add(1);
                          return true;
                        });
  slow.conn.query(SLOW);
  slow.conn.clear_progress();
  CHECK_EQ(first.load(), 0);
  CHECK(second.load() > 0);
}

ZU_TEST(an_interval_of_nothing_is_refused) {
  Slow slow;
  CHECK_THROWS_AS(zu::Exception,
                  slow.conn.on_progress(std::chrono::milliseconds{0},
                                        [](std::uint64_t, std::chrono::milliseconds) {
                                          return true;
                                        }));
}

ZU_TEST(an_interval_longer_than_the_statement_means_nothing_is_said_before_it_ends) {
  Slow slow;
  std::atomic<int> calls{0};
  slow.conn.on_progress(std::chrono::seconds{30}, [&](std::uint64_t, std::chrono::milliseconds) {
    calls.fetch_add(1);
    return true;
  });
  CHECK_EQ(slow.conn.query("RETURN 1 AS v").cell(0, 0).as_int(), 1);
  slow.conn.clear_progress();
  CHECK_EQ(calls.load(), 0);
}

ZU_TEST(a_connection_that_closes_with_a_watcher_on_it_lets_go_of_it) {
  /* Nothing to assert but that this does not crash: the std::function
   * outlives the connection unless something frees it, and the
   * destructor is what does. */
  auto db = zu::Database::memory();
  {
    auto conn = db.connect();
    conn.on_progress(std::chrono::milliseconds{1},
                     [](std::uint64_t, std::chrono::milliseconds) { return true; });
  }
  auto conn = db.connect();
  CHECK_EQ(conn.query("RETURN 1 AS v").cell(0, 0).as_int(), 1);
}

ZU_TEST(a_connection_that_is_moved_takes_its_watcher_with_it) {
  auto db = zu::Database::memory();
  auto conn = db.connect();
  std::atomic<int> calls{0};
  conn.on_progress(std::chrono::milliseconds{1}, [&](std::uint64_t, std::chrono::milliseconds) {
    calls.fetch_add(1);
    return true;
  });
  auto moved = std::move(conn);
  CHECK_EQ(moved.query("RETURN 1 AS v").cell(0, 0).as_int(), 1);
  moved.clear_progress();
}

ZU_TEST(interrupt_stops_a_statement_that_is_running) {
  Slow slow;
  bool stopped = false;
  try {
    slow.conn.on_progress(std::chrono::milliseconds{1},
                          [&](std::uint64_t, std::chrono::milliseconds) {
                            slow.conn.interrupt();
                            return true;
                          });
    slow.conn.query(SLOW);
  } catch (const zu::InterruptedError&) {
    stopped = true;
  }
  slow.conn.clear_progress();
  CHECK(stopped);
}

ZU_TEST(a_connection_says_how_many_rows_it_has_read) {
  Slow slow;
  const std::uint64_t before = slow.conn.rows_read();
  slow.conn.query("MATCH (p:Person) RETURN count(*) AS n");
  CHECK(slow.conn.rows_read() >= before);
}

ZU_TEST_MAIN()

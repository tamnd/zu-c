/* Watching a statement while it runs, and stopping one that is taking
 * too long.
 *
 * A watcher is called every so often with the rows read so far and the
 * time spent, and what it returns is whether to carry on. Returning
 * false stops the statement, and the caller sees it as an interrupted
 * error rather than as a wrong answer. The other way is interrupt()
 * from another thread, which is what a cancel button is wired to. */
#include <zu.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

/* A plain scan is too fast to be reported on at all, which is a nice
 * problem to have and an awkward one to demonstrate. A pair of patterns
 * with a predicate over them is a nested loop the planner cannot fold
 * into a count, and three thousand rows of it is about a third of a
 * second. */
constexpr std::size_t ROWS = 3000;
const char* const SLOW = "MATCH (a:Person), (b:Person) WHERE a.id < b.id RETURN count(*)";

std::vector<std::int64_t> ids() {
  std::vector<std::int64_t> out(ROWS);
  for (std::size_t i = 0; i < ROWS; ++i) {
    out[i] = static_cast<std::int64_t>(i);
  }
  return out;
}

}  // namespace

int main() {
  const std::vector<std::int64_t> buffer = ids();
  auto db = zu::Database::memory();
  auto conn = db.connect();
  auto frame = zu::Frame::create("Person", ROWS);
  frame.column("id", buffer);
  conn.register_frame(frame);

  std::uint64_t calls = 0;
  conn.on_progress(std::chrono::milliseconds{10},
                   [&](std::uint64_t rows, std::chrono::milliseconds elapsed) {
                     /* Both numbers only ever go up, which is what makes
                      * them safe to drive a progress bar with. Printing
                      * the first few is enough to show it. */
                     if (++calls <= 3) {
                       std::cout << "  " << rows << " rows after " << elapsed.count()
                                 << "ms\n";
                     }
                     return true;
                   });
  conn.query(SLOW);
  conn.clear_progress();
  std::cout << "finished after " << calls << " report(s)\n";

  /* A watcher that says no is a deadline, a row budget or a user who
   * pressed cancel. The statement stops where it is. */
  conn.on_progress(std::chrono::milliseconds{10},
                   [](std::uint64_t, std::chrono::milliseconds elapsed) {
                     return elapsed < std::chrono::milliseconds{50};
                   });
  try {
    conn.query(SLOW);
    std::cout << "finished inside the deadline\n";
  } catch (const zu::InterruptedError& e) {
    std::cout << "stopped by the watcher: " << e.what() << '\n';
  }
  conn.clear_progress();

  /* And the same from another thread, which is how a cancel button is
   * wired: interrupt() is the one call on a connection that is safe to
   * make while another thread is inside a statement. */
  std::atomic<bool> running{false};
  std::thread canceller([&] {
    while (!running.load()) {
      std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    conn.interrupt();
  });
  running.store(true);
  try {
    conn.query(SLOW);
    std::cout << "finished before the cancel landed\n";
  } catch (const zu::InterruptedError& e) {
    std::cout << "stopped from another thread: " << e.what() << '\n';
  }
  canceller.join();

  /* rows_read is the same counter the watcher is handed, for a host
   * that would rather poll than be called. */
  std::cout << conn.rows_read() << " rows read in all\n";
}

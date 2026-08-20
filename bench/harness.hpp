/* A timing harness small enough to read in one sitting.
 *
 * No dependency, because a benchmark that needs a package manager to
 * run is a benchmark nobody runs. What it does is the part that is easy
 * to get wrong: warm up before measuring, size the loop to the clock
 * rather than to a number somebody guessed, take the best of several
 * runs rather than the mean of one, and keep the compiler from deleting
 * the work.
 *
 * Best rather than mean is deliberate. The thing being measured has a
 * floor and no ceiling: the fastest run is the one with the least
 * interference from everything else on the machine, and averaging that
 * together with a run that got descheduled measures the scheduler. */
#ifndef ZU_BENCH_HARNESS_HPP
#define ZU_BENCH_HARNESS_HPP

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zb {

using Clock = std::chrono::steady_clock;

/* The one line of the harness that is not portable C++. It tells the
 * compiler a value escaped, so the work that produced it cannot be
 * folded away, and it costs nothing at runtime. */
template <class T>
inline void keep(T&& value) {
#if defined(__clang__) || defined(__GNUC__)
  asm volatile("" : : "r,m"(value) : "memory");
#else
  static volatile char sink;
  sink = *reinterpret_cast<const volatile char*>(&value);
#endif
}

struct Row {
  std::string name;
  double ns_per_op = 0;
  double items_per_sec = 0;
  std::uint64_t items_per_op = 0;
};

inline std::vector<Row>& rows() {
  static std::vector<Row> r;
  return r;
}

/* Runs f enough times to fill about a fifth of a second, five times
 * over, and keeps the fastest. items_per_op is how many rows one call
 * accounts for, which is what turns a duration into a throughput. */
template <class F>
void run(std::string_view name, std::uint64_t items_per_op, F&& f) {
  constexpr auto target = std::chrono::milliseconds{200};
  constexpr int repeats = 5;

  /* Warm up, and find out roughly how long one call takes while doing
   * it, so the measured loop is sized rather than guessed. */
  std::uint64_t iters = 1;
  for (;;) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
      f();
    }
    const auto spent = Clock::now() - start;
    if (spent >= std::chrono::milliseconds{20}) {
      const double scale = std::chrono::duration<double>(target).count() /
                           std::chrono::duration<double>(spent).count();
      const double wanted = static_cast<double>(iters) * scale;
      iters = wanted < 1.0 ? 1 : static_cast<std::uint64_t>(wanted);
      break;
    }
    if (iters > (std::uint64_t{1} << 40)) {
      break;
    }
    iters *= 2;
  }

  double best = 0;
  for (int r = 0; r < repeats; ++r) {
    const auto start = Clock::now();
    for (std::uint64_t i = 0; i < iters; ++i) {
      f();
    }
    const auto spent = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    const double per_op = spent / static_cast<double>(iters);
    if (best == 0 || per_op < best) {
      best = per_op;
    }
  }

  Row row;
  row.name = std::string(name);
  row.ns_per_op = best;
  row.items_per_op = items_per_op;
  row.items_per_sec =
      best > 0 ? static_cast<double>(items_per_op) * 1e9 / best : 0;
  rows().push_back(std::move(row));
}

inline void print(std::string_view title) {
#ifdef ZU_BENCH_UNOPTIMIZED
  std::printf(
      "\n(built without optimization, so these are not the numbers: configure with "
      "-DCMAKE_BUILD_TYPE=Release)\n");
#endif
  std::printf("\n%s\n", std::string(title).c_str());
  std::printf("%-44s %14s %16s\n", "", "per call", "rows per second");
  for (const Row& r : rows()) {
    if (r.ns_per_op >= 1e6) {
      std::printf("%-44s %11.2f ms", r.name.c_str(), r.ns_per_op / 1e6);
    } else if (r.ns_per_op >= 1e3) {
      std::printf("%-44s %11.2f us", r.name.c_str(), r.ns_per_op / 1e3);
    } else {
      std::printf("%-44s %11.2f ns", r.name.c_str(), r.ns_per_op);
    }
    if (r.items_per_op > 1) {
      std::printf(" %16.0f\n", r.items_per_sec);
    } else {
      std::printf(" %16s\n", "");
    }
  }
  rows().clear();
}

}  // namespace zb

#endif /* ZU_BENCH_HARNESS_HPP */

/* The test harness, which is a hundred lines and no dependency.
 *
 * A wrapper whose whole promise is that it needs nothing but a compiler
 * and libzu should not need a test framework to prove it, and a
 * contributor should be able to build the suite without fetching
 * anything. So: a registry, four macros, and a main.
 *
 * One executable per file, which is what gives ctest a name per file
 * and a failure a place. Inside a file the cases run in the order they
 * are written and each is independent of the others.
 *
 *   ZU_TEST(a_result_reads_its_columns) {
 *     auto conn = zu::Connection::memory();
 *     auto r = conn.query("RETURN 1 AS one");
 *     CHECK_EQ(r.rows(), 1u);
 *   }
 *
 * A failed check ends its case and no other. The runner takes an
 * optional argument, which is a substring: only the cases whose names
 * contain it run.
 */
#ifndef ZU_TEST_HARNESS_HPP
#define ZU_TEST_HARNESS_HPP

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace zt {

/* A check that did not hold. Thrown rather than recorded, because the
 * lines after a broken assumption are lines that read from a result
 * that is not what the case thought it was. */
struct Failure {
  std::string what;
};

struct Case {
  const char* name;
  const char* file;
  int line;
  void (*fn)();
};

inline std::vector<Case>& cases() {
  static std::vector<Case> all;
  return all;
}

struct Register {
  explicit Register(Case c) { cases().push_back(c); }
};

/* A value in a failure message. Enough shapes to cover what the tests
 * compare, and a hex dump of nothing for the rest, since a message
 * naming the line is already most of the way there. */
template <class T>
std::string show(const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_convertible_v<T, std::string_view>) {
    std::string out = "\"";
    out += std::string_view(value);
    out += '"';
    return out;
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_floating_point_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else if constexpr (std::is_pointer_v<T>) {
    return value == nullptr ? "null" : "a pointer";
  } else {
    return "<value>";
  }
}

[[noreturn]] inline void fail(const char* file, int line, std::string message) {
  std::string what = file;
  what += ':';
  what += std::to_string(line);
  what += ": ";
  what += message;
  throw Failure{std::move(what)};
}

/* A directory of its own for a case that needs files, removed when the
 * case ends however it ends. The name carries the case's, so a run left
 * behind by a crash says which case left it. */
class TempDir {
 public:
  explicit TempDir(std::string_view name) {
    static int counter = 0;
    std::string leaf = "zu-cpp-";
    leaf += name;
    leaf += '-';
    leaf += std::to_string(counter++);
    path_ = std::filesystem::temp_directory_path() / leaf;
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  /* A path inside it, as a string, because that is what the wrapper
   * takes and std::filesystem::path is a wide string on Windows. */
  std::string file(std::string_view leaf) const { return (path_ / leaf).string(); }

 private:
  std::filesystem::path path_;
};

inline int run(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  if (filter != nullptr && std::strcmp(filter, "--list") == 0) {
    for (const Case& c : cases()) {
      std::printf("%s\n", c.name);
    }
    return 0;
  }

  int ran = 0;
  int failed = 0;
  const auto started = std::chrono::steady_clock::now();
  for (const Case& c : cases()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) {
      continue;
    }
    ++ran;
    try {
      c.fn();
    } catch (const Failure& f) {
      ++failed;
      std::printf("FAIL %s\n     %s\n", c.name, f.what.c_str());
      continue;
    } catch (const std::exception& e) {
      ++failed;
      std::printf("FAIL %s\n     %s:%d: threw: %s\n", c.name, c.file, c.line, e.what());
      continue;
    } catch (...) {
      ++failed;
      std::printf("FAIL %s\n     %s:%d: threw something that is not an exception\n", c.name,
                  c.file, c.line);
      continue;
    }
    std::printf("ok   %s\n", c.name);
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
  std::printf("%d of %d passed in %lldms\n", ran - failed, ran, static_cast<long long>(ms));
  if (ran == 0) {
    std::printf("nothing matched, which is a failing suite rather than an empty one\n");
    return 1;
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace zt

#define ZU_TEST(NAME)                                                    \
  static void NAME();                                                    \
  static const ::zt::Register zt_register_##NAME{{#NAME, __FILE__, __LINE__, &NAME}}; \
  static void NAME()

#define CHECK(...)                                                       \
  do {                                                                   \
    if (!(__VA_ARGS__)) {                                                \
      ::zt::fail(__FILE__, __LINE__, "not true: " #__VA_ARGS__);         \
    }                                                                    \
  } while (false)

#define CHECK_EQ(A, B)                                                   \
  do {                                                                   \
    const auto zt_a = (A);                                               \
    const auto zt_b = (B);                                               \
    if (!(zt_a == zt_b)) {                                               \
      ::zt::fail(__FILE__, __LINE__,                                     \
                 std::string(#A " is ") + ::zt::show(zt_a) + " and " #B " is " + \
                     ::zt::show(zt_b));                                  \
    }                                                                    \
  } while (false)

#define CHECK_NE(A, B)                                                   \
  do {                                                                   \
    const auto zt_a = (A);                                               \
    const auto zt_b = (B);                                               \
    if (zt_a == zt_b) {                                                  \
      ::zt::fail(__FILE__, __LINE__,                                     \
                 std::string(#A " and " #B " are both ") + ::zt::show(zt_a)); \
    }                                                                    \
  } while (false)

/* The expression has to throw, and it has to throw that. A case that
 * says a bad statement is a SyntaxError and would pass on any exception
 * at all is a case that says nothing. */
#define CHECK_THROWS_AS(TYPE, ...)                                       \
  do {                                                                   \
    bool zt_threw = false;                                               \
    try {                                                                \
      (void)(__VA_ARGS__);                                               \
    } catch (const TYPE&) {                                              \
      zt_threw = true;                                                   \
    } catch (const std::exception& zt_e) {                               \
      ::zt::fail(__FILE__, __LINE__,                                     \
                 std::string(#__VA_ARGS__ " threw something that is not " #TYPE ": ") + \
                     zt_e.what());                                       \
    }                                                                    \
    if (!zt_threw) {                                                     \
      ::zt::fail(__FILE__, __LINE__, #__VA_ARGS__ " did not throw " #TYPE); \
    }                                                                    \
  } while (false)

#define ZU_TEST_MAIN() \
  int main(int argc, char** argv) { return ::zt::run(argc, argv); }

#endif /* ZU_TEST_HARNESS_HPP */

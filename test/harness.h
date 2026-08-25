/* The C test harness, which is the C++ one with the C++ taken out.
 *
 * This repository exists for the C ABI and until now every test in it
 * was C++. That is not a small gap. The wrapper is header-only, so a
 * C++ test compiles the header and exercises the ABI through it, and
 * anything the header gets right by accident of RAII is a thing the
 * suite never asked the ABI about. Misuse is exactly that category: a
 * destructor cannot run twice, a std::string cannot be a dangling
 * pointer, and a caller writing C has neither of those. So the cases
 * that matter most for this item are the ones no C++ file can write.
 *
 *   ZT_TEST(a_null_handle_is_misuse_and_not_a_crash) {
 *     ZT_CHECK_EQ(zu_conn_interrupt(NULL), ZU_MISUSE);
 *   }
 *
 *   ZT_MAIN(ZT_CASE(a_null_handle_is_misuse_and_not_a_crash))
 *
 * Three things about it are worth knowing before writing a case.
 *
 * The case list at the bottom is written out by hand, because C has no
 * way to run code before main that works on every compiler this
 * repository is willing to be built by. Forgetting to list a case is
 * still caught rather than silent: a case not in the list is a static
 * function nothing calls, which is -Wunused-function, which is
 * -Werror in CI.
 *
 * A check that does not hold returns from its case. There is no
 * longjmp and no exception, so a case that fails partway through does
 * not run its own cleanup and will leak whatever it was holding. That
 * is deliberate. A failing case has already found the news; a leak
 * report on top of it is noise, and the alternative is a goto ladder
 * in every case that would obscure the thing the case is about.
 *
 * The temporary directory is POSIX. The suite runs on Linux and macOS,
 * which is what CI covers, and the C cases are added to the build only
 * there.
 */
#ifndef ZU_TEST_HARNESS_H
#define ZU_TEST_HARNESS_H

#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Raised by a check that did not hold and read by the runner between
 * cases. One per process, because the cases run one at a time. */
static int zt_failed;

static inline void zt_report(const char *file, int line, const char *fmt, ...) {
  va_list args;
  printf("     %s:%d: ", file, line);
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
  printf("\n");
  zt_failed = 1;
}

/* Every check ends its case on the way out, so each of these is a
 * statement that can return and none of them is an expression. */
#define ZT_FAIL(...)                        \
  do {                                      \
    zt_report(__FILE__, __LINE__, __VA_ARGS__); \
    return;                                 \
  } while (0)

#define ZT_CHECK(EXPR)                                       \
  do {                                                       \
    if (!(EXPR)) {                                           \
      ZT_FAIL("not true: %s", #EXPR);                        \
    }                                                        \
  } while (0)

/* Integers, statuses and enums alike, all widened to one type so that
 * one macro covers what C++ needed a template for. Every value that
 * crosses this ABI as a count fits in a long long. */
#define ZT_CHECK_EQ(A, B)                                             \
  do {                                                                \
    const long long zt_a = (long long)(A);                            \
    const long long zt_b = (long long)(B);                            \
    if (zt_a != zt_b) {                                               \
      ZT_FAIL("%s is %lld and %s is %lld", #A, zt_a, #B, zt_b);        \
    }                                                                 \
  } while (0)

#define ZT_CHECK_NE(A, B)                                             \
  do {                                                                \
    const long long zt_a = (long long)(A);                            \
    const long long zt_b = (long long)(B);                            \
    if (zt_a == zt_b) {                                               \
      ZT_FAIL("%s and %s are both %lld", #A, #B, zt_a);                \
    }                                                                 \
  } while (0)

/* A pointer and a length against a C string, which is how every string
 * leaves this ABI. */
#define ZT_CHECK_STR(PTR, LEN, WANT)                                       \
  do {                                                                     \
    const char *zt_p = (PTR);                                              \
    const size_t zt_n = (size_t)(LEN);                                     \
    const char *zt_w = (WANT);                                             \
    if (zt_p == NULL) {                                                    \
      ZT_FAIL("%s is null and not \"%s\"", #PTR, zt_w);                    \
    }                                                                      \
    if (zt_n != strlen(zt_w) || memcmp(zt_p, zt_w, zt_n) != 0) {           \
      ZT_FAIL("%s is \"%.*s\" and not \"%s\"", #PTR, (int)zt_n, zt_p, zt_w); \
    }                                                                      \
  } while (0)

/* For the messages, which are prose and are allowed to be reworded.
 * The codes and the statuses are checked exactly; a message is checked
 * for the one word that says the case reached the failure it meant to
 * and not some other one. */
#define ZT_CHECK_HAS(PTR, LEN, WANT)                                       \
  do {                                                                     \
    const char *zt_p = (PTR);                                              \
    const size_t zt_n = (size_t)(LEN);                                     \
    const char *zt_w = (WANT);                                             \
    if (zt_p == NULL) {                                                    \
      ZT_FAIL("%s is null and does not hold \"%s\"", #PTR, zt_w);          \
    }                                                                      \
    if (zt_n < strlen(zt_w) || zt_find(zt_p, zt_n, zt_w) == 0) {           \
      ZT_FAIL("%s is \"%.*s\" and does not hold \"%s\"", #PTR, (int)zt_n,  \
              zt_p, zt_w);                                                 \
    }                                                                      \
  } while (0)

/* strstr over a counted string, since nothing out of this ABI is
 * promised to be NUL-terminated. */
static inline int zt_find(const char *haystack, size_t n, const char *needle) {
  const size_t m = strlen(needle);
  size_t i;
  if (m > n) {
    return 0;
  }
  for (i = 0; i + m <= n; i++) {
    if (memcmp(haystack + i, needle, m) == 0) {
      return 1;
    }
  }
  return 0;
}

/* A directory of its own for a case that wants files, found by making
 * it rather than by picking a name and clearing whatever is there, for
 * the reason written up at length in harness.hpp: two builds of the
 * same suite run at once under ctest -j and a remove_all here deletes
 * the other one's database out from under it. */
typedef struct zt_tmpdir {
  char path[512];
} zt_tmpdir;

static inline int zt_tmpdir_open(zt_tmpdir *dir, const char *name) {
  static int counter;
  const char *root = getenv("TMPDIR");
  int tries;
  if (root == NULL || root[0] == '\0') {
    root = "/tmp";
  }
  for (tries = 0; tries < 4096; tries++) {
    /* getpid in the name as well as the counter, because the counter
     * restarts in each process and two processes racing on mkdir is
     * only safe, not free: without the pid they would collide four
     * thousand times before either finished. */
    snprintf(dir->path, sizeof dir->path, "%s/zu-c-%s-%ld-%d", root, name,
             (long)getpid(), counter++);
    if (mkdir(dir->path, 0700) == 0) {
      return 1;
    }
  }
  dir->path[0] = '\0';
  return 0;
}

/* One level deep, which is what the engine writes: a database file and
 * whatever sidecars it keeps beside it, never a subdirectory. A file
 * that will not go is left rather than chased, and the directory then
 * stays too, which is a stale directory in the temporary area and not
 * a failed test. */
static inline void zt_tmpdir_close(zt_tmpdir *dir) {
  DIR *d;
  struct dirent *entry;
  if (dir->path[0] == '\0') {
    return;
  }
  d = opendir(dir->path);
  if (d != NULL) {
    while ((entry = readdir(d)) != NULL) {
      /* Wide enough for the directory and the longest name a dirent
       * can carry, so that nothing here is a truncation the compiler
       * has to be told to allow. */
      char leaf[sizeof dir->path + 258];
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      snprintf(leaf, sizeof leaf, "%s/%s", dir->path, entry->d_name);
      (void)unlink(leaf);
    }
    closedir(d);
  }
  (void)rmdir(dir->path);
  dir->path[0] = '\0';
}

static inline void zt_tmpdir_file(const zt_tmpdir *dir, const char *leaf, char *out,
                                  size_t out_len) {
  snprintf(out, out_len, "%s/%s", dir->path, leaf);
}

/* The lowest descriptor nobody is using, which on both platforms this
 * runs on is the number the next open would be given. A loop that
 * opens and closes a thousand connections should end where it started;
 * one that ends higher gave a descriptor back to nobody.
 *
 * A number rather than a count, because counting the entries under
 * /proc/self/fd is Linux only and this is not. */
static inline int zt_next_fd(void) {
  int fd = open("/dev/null", O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  close(fd);
  return fd;
}

/* How long a fixture that exists in order to be slow has to last.
 *
 * Two cases here need a statement that is still running when something
 * else happens to it: the progress watcher has to fire while it runs,
 * and the interrupt has to land before it ends. Both get that by
 * counting pairs over three thousand people, which takes about a third
 * of a second and is nothing.
 *
 * There used to be a ZU_TEST_ROWS here that shrank the number, and the
 * valgrind job set it to three hundred on the reasoning that memcheck
 * already runs the machine forty times slower and that is another way
 * of getting a statement that lasts. It is not. The work is pairs, so
 * a tenth of the rows is a hundredth of the statement, and a hundredth
 * slowed by forty is four tenths: under memcheck at three hundred rows
 * the statement was shorter than it is here at three thousand. The
 * watcher never fired, the case that asserts it fired failed, and the
 * case that asserts an interrupt lands was next in the same job and
 * would have failed the same way.
 *
 * So there is no knob. The number is written where it is used, the two
 * cases cost the valgrind job the minute they cost it, and what they
 * check is a thing that happened rather than a thing that had time to.
 */

typedef struct zt_case {
  const char *name;
  void (*fn)(void);
} zt_case;

#define ZT_TEST(NAME) static void NAME(void)
#define ZT_CASE(NAME) {#NAME, NAME}

static inline int zt_run(const zt_case *all, size_t n, int argc, char **argv) {
  const char *filter = argc > 1 ? argv[1] : NULL;
  clock_t started;
  size_t i;
  int ran = 0;
  int failures = 0;

  if (filter != NULL && strcmp(filter, "--list") == 0) {
    for (i = 0; i < n; i++) {
      printf("%s\n", all[i].name);
    }
    return 0;
  }

  started = clock();
  for (i = 0; i < n; i++) {
    if (filter != NULL && strstr(all[i].name, filter) == NULL) {
      continue;
    }
    ran++;
    zt_failed = 0;
    printf("     running %s\n", all[i].name);
    fflush(stdout);
    all[i].fn();
    if (zt_failed) {
      failures++;
      printf("FAIL %s\n", all[i].name);
    } else {
      printf("ok   %s\n", all[i].name);
    }
  }
  printf("%d of %d passed in %ldms\n", ran - failures, ran,
         (long)((clock() - started) * 1000 / CLOCKS_PER_SEC));
  if (ran == 0) {
    printf("nothing matched, which is a failing suite rather than an empty one\n");
    return 1;
  }
  return failures == 0 ? 0 : 1;
}

#define ZT_MAIN(...)                                                     \
  int main(int argc, char **argv) {                                      \
    static const zt_case zt_all[] = {__VA_ARGS__};                       \
    return zt_run(zt_all, sizeof zt_all / sizeof zt_all[0], argc, argv); \
  }

#endif /* ZU_TEST_HARNESS_H */

/* The concurrency the ABI promises, run concurrently.
 *
 * The header opens with the rule the whole threading story rests on: a
 * zu_database "holds no descriptor and no cache, so it is thread-safe
 * and shareable", a zu_conn "is the state that cannot be shared", and
 * "a host that queries from four threads opens one database and
 * connects four times".
 *
 * Nothing here was running that. misuse.c checks what happens when the
 * rule is broken, which it can do from one thread because the progress
 * watcher supplies the second. This file checks that the rule works
 * when it is kept, which needs real threads, and it is the only file in
 * the suite that starts any.
 *
 * That is also what gives the thread sanitizer something to say, and
 * why this is a file of its own rather than three more cases in
 * misuse.c: CI runs this one under TSan and the rest under address and
 * undefined behaviour, and those two instrumentations cannot be linked
 * into one binary.
 *
 * What TSan can and cannot see here is worth being straight about.
 * libzu is not instrumented, so a race entirely inside the engine's own
 * memory is invisible to it. What it does see is every pthread
 * primitive, because those are intercepted rather than compiled, and
 * every access this file makes to its own memory from more than one
 * thread. So it covers the half a host can get wrong, which is the half
 * this repository is here to be right about.
 */
#include <zu.h>

#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"

#define WORKERS 8
#define ROUNDS 64
#define PEOPLE 512

/* One array of people, read by every thread and written by none, which
 * is the shape a host holding a dataframe arrives in. The header says
 * of registered frames that "nothing stops the connections registering
 * the same memory twice", and this is that sentence run eight ways at
 * once. */
static int64_t ids[PEOPLE];

/* What one worker was given and what it found. Every field is written
 * by its own thread and read by the main one after a join, which is an
 * ordering pthread_join establishes and TSan knows about. */
struct worker {
  pthread_t id;
  zu_database *db;
  int index;
  int rounds;
  int64_t answer;
  zu_status failed_at;
  const char *stage;
};

static void *count_people(void *arg) {
  struct worker *w = (struct worker *)arg;
  zu_conn *conn = NULL;
  zu_frame *frame = NULL;
  int i;

  /* A connection of its own, out of the shared database, which is the
   * documented arrangement. */
  w->stage = "connect";
  w->failed_at = zu_connect(w->db, &conn, NULL);
  if (w->failed_at != ZU_OK) {
    return NULL;
  }
  /* And a frame of its own over the shared array, because a frame is a
   * connection-shaped thing rather than a database-shaped one: "a frame
   * is used from one thread, like the connection it registers on". The
   * memory underneath is what is shared. */
  w->stage = "frame";
  w->failed_at = zu_frame_new_z("Person", PEOPLE, NULL, NULL, &frame, NULL);
  if (w->failed_at == ZU_OK) {
    w->stage = "column";
    w->failed_at = zu_frame_col_int(frame, "id", 2, ids, PEOPLE, 64, 1, 1, ZU_FRAME_PLAIN, NULL);
  }
  if (w->failed_at == ZU_OK) {
    w->stage = "register";
    w->failed_at = zu_conn_register(conn, frame, NULL);
  }
  if (w->failed_at != ZU_OK) {
    zu_frame_free(frame);
    zu_conn_close(conn);
    return NULL;
  }

  w->stage = "query";
  for (i = 0; i < ROUNDS; i++) {
    zu_result *res = NULL;
    const zu_value *cell = NULL;
    int64_t n = 0;
    zu_status st = zu_query_z(conn, "MATCH (p:Person) RETURN count(*) AS n", &res, NULL);
    if (st != ZU_OK) {
      w->failed_at = st;
      break;
    }
    if (zu_result_cell(res, 0, 0, &cell) != ZU_OK || zu_value_i64(cell, &n) != ZU_OK) {
      w->stage = "cell";
      w->failed_at = ZU_MISUSE;
      zu_result_free(res);
      break;
    }
    zu_result_free(res);
    w->answer = n;
    w->rounds++;
  }
  zu_conn_close(conn);
  zu_frame_free(frame);
  return NULL;
}

ZT_TEST(eight_threads_with_a_connection_each_read_one_database_and_agree) {
  /* The supported shape, run hard enough to be worth running: one
   * database handle shared by eight threads, a connection made inside
   * each of them, and sixty four statements down each connection.
   *
   * All eight have to finish, and all eight have to answer 512, because
   * nothing is writing. A wrong count from one thread and a right one
   * from the other seven is the failure this is looking for, and it is
   * the failure a locked-per-connection design is meant to make
   * impossible.
   *
   * A frame rather than a file, so the threads contend on the engine's
   * own structures rather than queue on a disk. */
  struct worker workers[WORKERS];
  zu_database *db = NULL;
  int i;

  for (i = 0; i < PEOPLE; i++) {
    ids[i] = i;
  }
  memset(workers, 0, sizeof workers);

  ZT_CHECK_EQ(zu_database_memory(NULL, &db, NULL), ZU_OK);

  for (i = 0; i < WORKERS; i++) {
    workers[i].db = db;
    workers[i].index = i;
    workers[i].answer = -1;
    workers[i].stage = "unstarted";
  }
  for (i = 0; i < WORKERS; i++) {
    if (pthread_create(&workers[i].id, NULL, count_people, &workers[i]) != 0) {
      ZT_FAIL("could not start worker %d", i);
    }
  }
  for (i = 0; i < WORKERS; i++) {
    pthread_join(workers[i].id, NULL);
  }

  for (i = 0; i < WORKERS; i++) {
    if (workers[i].failed_at != ZU_OK) {
      ZT_FAIL("worker %d stopped at %s with status %d after %d rounds", i, workers[i].stage,
              (int)workers[i].failed_at, workers[i].rounds);
    }
    ZT_CHECK_EQ(workers[i].rounds, ROUNDS);
    ZT_CHECK_EQ(workers[i].answer, PEOPLE);
  }

  zu_database_close(db);
}

/* A connection handed from one thread to the next, one at a time. */
struct handoff {
  zu_conn *conn;
  int64_t answer;
  zu_status status;
};

static void *use_the_connection_once(void *arg) {
  struct handoff *h = (struct handoff *)arg;
  zu_result *res = NULL;
  const zu_value *cell = NULL;
  h->status = zu_query_z(h->conn, "RETURN 42 AS n", &res, NULL);
  if (h->status == ZU_OK) {
    if (zu_result_cell(res, 0, 0, &cell) != ZU_OK || zu_value_i64(cell, &h->answer) != ZU_OK) {
      h->status = ZU_MISUSE;
    }
  }
  zu_result_free(res);
  return NULL;
}

ZT_TEST(a_connection_moves_between_threads_as_long_as_only_one_holds_it) {
  /* "A connection may move between threads but must not be used from
   * two at once." The second half of that sentence is the famous one
   * and the first half is the one a pool depends on: a worker takes a
   * connection off a queue, uses it, and puts it back for a different
   * worker to take.
   *
   * So the connection is opened here, used on a thread, joined, used on
   * another thread, joined, and closed here. Nothing shares it at any
   * instant and every handoff is a join, which is the discipline a pool
   * has to keep and the one TSan can check. */
  zu_conn *conn = NULL;
  int i;

  ZT_CHECK_EQ(zu_memory(&conn, NULL), ZU_OK);
  for (i = 0; i < 16; i++) {
    struct handoff h;
    pthread_t worker;
    memset(&h, 0, sizeof h);
    h.conn = conn;
    h.status = ZU_MISUSE;
    if (pthread_create(&worker, NULL, use_the_connection_once, &h) != 0) {
      zu_conn_close(conn);
      ZT_FAIL("could not start the worker for pass %d", i);
    }
    pthread_join(worker, NULL);
    if (h.status != ZU_OK) {
      zu_conn_close(conn);
      ZT_FAIL("pass %d on its own thread answered %d", i, (int)h.status);
    }
    ZT_CHECK_EQ(h.answer, 42);
  }
  /* And it belongs to whoever holds it, including this thread, which is
   * where it started. */
  {
    zu_result *res = NULL;
    const zu_value *cell = NULL;
    int64_t n = 0;
    ZT_CHECK_EQ(zu_query_z(conn, "RETURN 7 AS n", &res, NULL), ZU_OK);
    ZT_CHECK_EQ(zu_result_cell(res, 0, 0, &cell), ZU_OK);
    ZT_CHECK_EQ(zu_value_i64(cell, &n), ZU_OK);
    ZT_CHECK_EQ(n, 7);
    zu_result_free(res);
  }
  zu_conn_close(conn);
}

/* One connection and two threads, which is the thing the header tells a
 * host not to do. The second thread hammers the connection while the
 * first runs statements on it, and every call it makes has to come back
 * with a status rather than with a corrupted cache. */
struct sharer {
  zu_conn *conn;
  int calls;
  int refused;
  int allowed;
  int other;
  zu_status worst;
};

/* The stop flag goes through a mutex rather than being a volatile int,
 * because this file is compiled under TSan, where a plain flag written
 * on one thread and read on another is a data race and is reported as
 * one. It would be a race without TSan too; the sanitizer only makes it
 * visible, and a suite that reports races cannot afford to contain
 * one. */
static pthread_mutex_t stop_lock = PTHREAD_MUTEX_INITIALIZER;
static int stop_now;

static int should_stop(void) {
  int stop;
  pthread_mutex_lock(&stop_lock);
  stop = stop_now;
  pthread_mutex_unlock(&stop_lock);
  return stop;
}

static void set_stop(int value) {
  pthread_mutex_lock(&stop_lock);
  stop_now = value;
  pthread_mutex_unlock(&stop_lock);
}

static void *share_the_connection(void *arg) {
  struct sharer *s = (struct sharer *)arg;
  while (!should_stop()) {
    zu_result *res = NULL;
    zu_status st = zu_query_z(s->conn, "RETURN 1 AS one", &res, NULL);
    zu_result_free(res);
    s->calls++;
    if (st == ZU_MISUSE_CONCURRENT) {
      s->refused++;
    } else if (st == ZU_OK) {
      s->allowed++;
    } else {
      s->other++;
      s->worst = st;
    }
  }
  return NULL;
}

ZT_TEST(one_connection_in_two_threads_is_refused_rather_than_raced) {
  /* misuse.c gets this deterministically from inside the progress
   * watcher, which is the version that always fires. This is the same
   * rule the way a host meets it by accident: two threads genuinely
   * racing, for as long as it takes the first to run five hundred
   * statements.
   *
   * The assertion cannot be that every call was refused, because a call
   * that arrives while the connection is idle is a call on an idle
   * connection and is allowed. What it can be, and what the header
   * promises, is that nothing comes back as any other kind of failure
   * and that both threads keep getting right answers. A raced cache
   * shows up as a wrong number or a crash rather than as a status. */
  zu_conn *conn = NULL;
  struct sharer state;
  pthread_t other;
  int i;

  memset(&state, 0, sizeof state);
  set_stop(0);

  ZT_CHECK_EQ(zu_memory(&conn, NULL), ZU_OK);
  state.conn = conn;

  if (pthread_create(&other, NULL, share_the_connection, &state) != 0) {
    zu_conn_close(conn);
    ZT_FAIL("could not start the second thread");
  }

  for (i = 0; i < 500; i++) {
    zu_result *res = NULL;
    const zu_value *cell = NULL;
    int64_t n = 0;
    zu_status st = zu_query_z(conn, "RETURN 41 + 1 AS n", &res, NULL);
    if (st == ZU_MISUSE_CONCURRENT) {
      /* This thread lost the toss, which is allowed and is the same
       * answer the other thread gets when it loses. */
      continue;
    }
    if (st != ZU_OK) {
      set_stop(1);
      pthread_join(other, NULL);
      zu_conn_close(conn);
      ZT_FAIL("a statement on the owning thread answered %d", (int)st);
    }
    if (zu_result_cell(res, 0, 0, &cell) != ZU_OK || zu_value_i64(cell, &n) != ZU_OK || n != 42) {
      zu_result_free(res);
      set_stop(1);
      pthread_join(other, NULL);
      zu_conn_close(conn);
      ZT_FAIL("a statement run beside another thread answered %lld", (long long)n);
    }
    zu_result_free(res);
  }

  set_stop(1);
  pthread_join(other, NULL);

  ZT_CHECK(state.calls > 0);
  /* Nothing but the two expected answers. Anything else would mean the
   * intruding thread got far enough in for the engine to fail the work
   * rather than turning it away at the door. */
  if (state.other != 0) {
    zu_conn_close(conn);
    ZT_FAIL("%d of %d calls from the second thread answered %d, which is neither ok nor concurrent",
            state.other, state.calls, (int)state.worst);
  }

  /* And the connection is fit to use afterwards, which is the whole
   * point of refusing rather than racing. */
  {
    zu_result *res = NULL;
    const zu_value *cell = NULL;
    int64_t n = 0;
    ZT_CHECK_EQ(zu_query_z(conn, "RETURN 7 AS n", &res, NULL), ZU_OK);
    ZT_CHECK_EQ(zu_result_cell(res, 0, 0, &cell), ZU_OK);
    ZT_CHECK_EQ(zu_value_i64(cell, &n), ZU_OK);
    ZT_CHECK_EQ(n, 7);
    zu_result_free(res);
  }
  zu_conn_close(conn);
}

/* The one call the header says is meant to cross threads. */
struct interrupter {
  zu_conn *conn;
  zu_status status;
};

/* Waiting on nothing for a while, which is the sleep that needs no
 * feature test macro. usleep was withdrawn from POSIX in 2008 and glibc
 * hides it from a file compiled at -std=c11; nanosleep is there but its
 * declaration is behind __USE_POSIX199309, so reaching it means
 * defining _POSIX_C_SOURCE at the top of the file, which changes what
 * every other header here declares and does so differently on macOS
 * than on glibc. poll is declared by poll.h on both without asking for
 * anything, and a poll of no descriptors is a timer. */
static void sleep_ms(int ms) { poll(NULL, 0, ms); }

static void *interrupt_soon(void *arg) {
  struct interrupter *state = (struct interrupter *)arg;
  /* Long enough that the statement is certainly running, short enough
   * that it is certainly not finished. The statement below takes about
   * a third of a second uninstrumented and longer under a sanitizer,
   * which is the direction it is safe to be wrong in. */
  sleep_ms(50);
  state->status = zu_conn_interrupt(state->conn);
  return NULL;
}

ZT_TEST(an_interrupt_from_another_thread_stops_the_statement_and_not_the_connection) {
  /* zu_conn_interrupt is documented as the exception to the sharing
   * rule, and the reason given is worth checking rather than trusting:
   * "a cancellation that had to wait for the connection to be free
   * could only arrive after the statement it was meant to stop".
   *
   * So the interrupt has to be accepted while the connection is busy,
   * which is exactly what every other call is refused for; the
   * statement has to come back ZU_INTERRUPTED rather than failing; and
   * the connection has to run the next statement normally, which is
   * what the header says makes this different from closing it. */
  static int64_t many[3000];
  const uint64_t rows = zt_rows(3000);
  zu_database *db = NULL;
  zu_conn *conn = NULL;
  zu_frame *frame = NULL;
  zu_result *res = NULL;
  struct interrupter state;
  pthread_t other;
  uint64_t i;

  for (i = 0; i < rows; i++) {
    many[i] = (int64_t)i;
  }
  memset(&state, 0, sizeof state);

  ZT_CHECK_EQ(zu_database_memory(NULL, &db, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_connect(db, &conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_frame_new_z("Person", rows, NULL, NULL, &frame, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_frame_col_int(frame, "id", 2, many, rows, 64, 1, 1, ZU_FRAME_PLAIN, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_conn_register(conn, frame, NULL), ZU_OK);

  state.conn = conn;
  state.status = ZU_MISUSE;
  if (pthread_create(&other, NULL, interrupt_soon, &state) != 0) {
    ZT_FAIL("could not start the interrupting thread");
  }

  /* Every pair of people with a predicate over them, which the planner
   * cannot fold into a count, so it runs long enough to be stopped in
   * the middle of. */
  ZT_CHECK_EQ(
      zu_query_z(conn, "MATCH (a:Person), (b:Person) WHERE a.id < b.id RETURN count(*)", &res, NULL),
      ZU_INTERRUPTED);
  ZT_CHECK(res == NULL);
  pthread_join(other, NULL);

  /* Accepted while the connection was busy, rather than turned away the
   * way every other call would have been. */
  ZT_CHECK_EQ(state.status, ZU_OK);

  /* And nothing is wrong with the connection. */
  {
    const zu_value *cell = NULL;
    int64_t n = 0;
    ZT_CHECK_EQ(zu_query_z(conn, "RETURN 7 AS n", &res, NULL), ZU_OK);
    ZT_CHECK_EQ(zu_result_cell(res, 0, 0, &cell), ZU_OK);
    ZT_CHECK_EQ(zu_value_i64(cell, &n), ZU_OK);
    ZT_CHECK_EQ(n, 7);
    zu_result_free(res);
  }

  zu_conn_close(conn);
  zu_frame_free(frame);
  zu_database_close(db);
}

ZT_MAIN(ZT_CASE(eight_threads_with_a_connection_each_read_one_database_and_agree),
        ZT_CASE(a_connection_moves_between_threads_as_long_as_only_one_holds_it),
        ZT_CASE(one_connection_in_two_threads_is_refused_rather_than_raced),
        ZT_CASE(an_interrupt_from_another_thread_stops_the_statement_and_not_the_connection))

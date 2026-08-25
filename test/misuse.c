/* Deliberately wrong programs, and what each one is told.
 *
 * The scorecard item asks for the misuse and lifecycle suite of dx/15
 * section 4: wrong programs, and for each one no crash, no leak, and a
 * clear message. This is that suite, and it is in C rather than C++ on
 * purpose, because most of what it checks cannot be written in C++ at
 * all. A destructor does not run twice. A zu::Connection has no null
 * state to pass. A Result cannot outlive its Connection by accident,
 * because the wrapper arranged that it does not. Every one of those is
 * a promise the ABI makes and the wrapper keeps for you, and a suite
 * that only ever reaches the ABI through the wrapper has never once
 * asked the ABI to keep it.
 *
 * So these are the cases a C host writes by mistake. A handle used
 * after it was closed, a null where a pointer was wanted, a result
 * outliving the connection it came from, a loop that opens and closes
 * a thousand times, a transaction abandoned rather than ended, a call
 * back into the library from the one thread the library said not to
 * call it from.
 *
 * The bar for each is the same three things. It does not crash. It
 * gives back what it took. And it says something a person can act on,
 * which for this ABI means a status that names the kind of mistake:
 * ZU_MISUSE for a call that was wrong, ZU_MISUSE_CLOSED for one that
 * came too late, ZU_MISUSE_CONCURRENT for one that came from the wrong
 * thread. A crash is the failure this file exists to prevent, and a
 * status of ZU_ERROR where one of those three belongs is the near miss
 * that turns into one.
 */
#include <zu.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "harness.h"

/* Three people on disk at the path given, since a case that wants a
 * table has to bulk load one: there is no DDL that makes a node table.
 * Returns zero having already reported, so a caller writes
 * `if (!people(path)) return;`. */
static int people(const char *path) {
  zu_loader *l = NULL;
  zu_error *err = NULL;
  static const int64_t ids[3] = {1, 2, 3};
  static const char *const names[3] = {"ada", "grace", "alan"};
  if (zu_loader_create_z(path, &l, &err) != ZU_OK) {
    zu_error_free(err);
    zt_report(__FILE__, __LINE__, "the fixture could not make a database at %s", path);
    return 0;
  }
  if (zu_loader_table_z(l, "Person", "Knows", 3, NULL) != ZU_OK ||
      zu_loader_col_i64(l, "id", 2, ids, 3, NULL) != ZU_OK ||
      zu_loader_col_str_z(l, "name", names, 3, NULL) != ZU_OK ||
      zu_loader_finish(l, NULL) != ZU_OK) {
    zu_loader_free(l);
    zt_report(__FILE__, __LINE__, "the fixture could not load three people");
    return 0;
  }
  zu_loader_free(l);
  return 1;
}

ZT_TEST(a_wrong_program_is_told_what_is_wrong) {
  /* The whole of the bar in one case. A statement that will not parse
   * comes back as a status naming the kind of failure, an error the
   * caller can read the condition out of, and no result. */
  zu_conn *conn = NULL;
  zu_result *res = NULL;
  zu_error *err = NULL;
  const char *code = NULL;
  const char *msg = NULL;
  const char *url = NULL;
  size_t len = 0;
  uint32_t line = 0;
  uint32_t column = 0;

  ZT_CHECK_EQ(zu_memory(&conn, &err), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(conn, "MATCH (p:Person RETURN p", &res, &err), ZU_ERROR);
  ZT_CHECK(res == NULL);
  ZT_CHECK(err != NULL);

  ZT_CHECK_EQ(zu_error_status(err), ZU_ERROR);
  code = zu_error_code(err, &len);
  ZT_CHECK_STR(code, len, "42001");
  url = zu_error_doc_url(err, &len);
  ZT_CHECK_STR(url, len, "https://zu.dev/docs/errors/42001");
  msg = zu_error_message(err, &len);
  ZT_CHECK(msg != NULL && len > 0);
  ZT_CHECK_EQ(zu_error_position(err, &line, &column), ZU_OK);
  ZT_CHECK_EQ(line, 1);
  ZT_CHECK_EQ(column, 17);

  zu_error_free(err);
  zu_conn_close(conn);
}

ZT_TEST(a_call_that_failed_wrote_its_out_parameter_rather_than_leaving_it) {
  /* The header's promise, and the one a C host is most likely to be
   * betting on without knowing it: "The out-parameter is written on
   * every path, NULL when there is nothing to point at, so a caller who
   * ignores the status is never left holding a pointer from the call
   * before."
   *
   * That last clause is the danger. A caller who reuses one zu_result *
   * across a loop and checks it for null instead of checking the status
   * reads the previous iteration's result if a call ever skips writing.
   * So this asks for a real result first, then fails, then looks. */
  zu_conn *conn = NULL;
  zu_result *res = NULL;
  zu_stmt *stmt = NULL;

  ZT_CHECK_EQ(zu_memory(&conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(conn, "RETURN 1 AS one", &res, NULL), ZU_OK);
  ZT_CHECK(res != NULL);
  zu_result_free(res);

  /* res still holds the address of the result just freed. */
  ZT_CHECK_EQ(zu_query_z(conn, "RETURN nope", &res, NULL), ZU_ERROR);
  ZT_CHECK(res == NULL);

  ZT_CHECK_EQ(zu_prepare_z(conn, "RETURN 1 AS one", &stmt, NULL), ZU_OK);
  ZT_CHECK(stmt != NULL);
  zu_stmt_close(stmt);
  ZT_CHECK_EQ(zu_prepare_z(conn, "MATCH (", &stmt, NULL), ZU_ERROR);
  ZT_CHECK(stmt == NULL);

  zu_conn_close(conn);
}

ZT_TEST(an_error_nobody_asked_for_is_dropped_and_the_status_still_says) {
  /* Passing NULL for the zu_error ** is allowed and is what a host that
   * only wants to know whether the call worked should write. The thing
   * to check is that it is a discard rather than a different code path:
   * the status has to be the same status. */
  zu_conn *conn = NULL;
  zu_result *res = NULL;
  zu_error *err = NULL;

  ZT_CHECK_EQ(zu_memory(&conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(conn, "RETURN 1 / 0", &res, NULL), ZU_ERROR);
  ZT_CHECK(res == NULL);
  ZT_CHECK_EQ(zu_query_z(conn, "RETURN 1 / 0", &res, &err), ZU_ERROR);
  ZT_CHECK(err != NULL);
  zu_error_free(err);
  zu_conn_close(conn);
}

ZT_TEST(a_null_handle_is_misuse_rather_than_a_crash) {
  /* The commonest C mistake there is: the call before this one failed,
   * the caller did not look, and now a null handle is being passed to
   * everything downstream. Every one of these has to answer rather than
   * dereference.
   *
   * The accessors are here too, because they take no zu_error ** and
   * their whole error channel is the status. */
  zu_result *res = NULL;
  zu_error *err = NULL;
  int64_t out = 0;
  uint64_t rows = 0;
  int32_t flag = 0;
  size_t len = 0;

  ZT_CHECK_EQ(zu_query_z(NULL, "RETURN 1 AS one", &res, &err), ZU_MISUSE);
  ZT_CHECK(res == NULL);
  ZT_CHECK_EQ(zu_prepare_z(NULL, "RETURN 1 AS one", NULL, NULL), ZU_MISUSE);
  ZT_CHECK_EQ(zu_conn_interrupt(NULL), ZU_MISUSE);
  ZT_CHECK_EQ(zu_conn_rows_read(NULL, &rows), ZU_MISUSE);
  ZT_CHECK_EQ(zu_begin(NULL, 0, NULL), ZU_MISUSE);
  ZT_CHECK_EQ(zu_commit(NULL, NULL), ZU_MISUSE);
  ZT_CHECK_EQ(zu_rollback(NULL, NULL), ZU_MISUSE);
  ZT_CHECK_EQ(zu_conn_in_transaction(NULL, &flag), ZU_MISUSE);
  ZT_CHECK_EQ(zu_execute(NULL, &res, NULL), ZU_MISUSE);
  ZT_CHECK(res == NULL);
  ZT_CHECK_EQ(zu_bind_i64_z(NULL, "n", 1), ZU_MISUSE);
  ZT_CHECK_EQ(zu_result_cell_type(NULL, 0, 0, &flag), ZU_MISUSE);
  ZT_CHECK_EQ(zu_value_i64(NULL, &out), ZU_MISUSE);

  /* A null error answers rather than crashing too, and answers the
   * value the header named for it rather than a plausible one. */
  ZT_CHECK_EQ(zu_error_severity(NULL), -1);
  ZT_CHECK_EQ(zu_error_retryable(NULL), -1);
  ZT_CHECK_EQ(zu_error_position(NULL, NULL, NULL), ZU_MISUSE);
  ZT_CHECK(zu_error_code(NULL, &len) == NULL);
  ZT_CHECK(zu_error_message(NULL, &len) == NULL);
}

ZT_TEST(giving_back_nothing_is_allowed_everywhere_it_is_written_down) {
  /* "Every *_free and *_close call here is a no-op on NULL." A C host
   * leans on that in every cleanup path it writes, because the
   * alternative is a null check per line, and a single one of these
   * that dereferenced would turn every failed open in every program
   * into a crash on the way out. */
  zu_conn_close(NULL);
  zu_close(NULL);
  zu_database_close(NULL);
  zu_stmt_close(NULL);
  zu_result_free(NULL);
  zu_error_free(NULL);
  zu_loader_free(NULL);
  zu_frame_free(NULL);
}

ZT_TEST(a_statement_used_after_its_connection_closed_says_so_rather_than_following_a_pointer) {
  /* The lifetime rule with a name of its own in the header: statements
   * belong to the connection they were prepared on, and using one after
   * that connection closes answers ZU_MISUSE_CLOSED rather than
   * following a dangling pointer.
   *
   * Worth a case because getting it wrong is silent. A freed connection
   * whose memory has not been reused yet reads as a live one, so the
   * version of this that follows the pointer passes on the machine it
   * was written on and crashes in production. */
  zu_conn *conn = NULL;
  zu_stmt *stmt = NULL;
  zu_result *res = NULL;

  ZT_CHECK_EQ(zu_memory(&conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_prepare_z(conn, "RETURN $n AS n", &stmt, NULL), ZU_OK);
  zu_conn_close(conn);

  ZT_CHECK_EQ(zu_bind_i64_z(stmt, "n", 1), ZU_MISUSE_CLOSED);
  ZT_CHECK_EQ(zu_execute(stmt, &res, NULL), ZU_MISUSE_CLOSED);
  ZT_CHECK(res == NULL);

  /* And closing it, which is the one call that still has to work. */
  zu_stmt_close(stmt);
}

ZT_TEST(a_result_outlives_its_connection_because_it_owns_its_rows) {
  /* The other half of the same promise, and the one that is not misuse
   * at all: "Results own their rows outright, so a result stays
   * readable after its connection has gone back to a pool." A host
   * pooling connections returns one the moment a query answers, and
   * everything it read afterwards is either fine or is a read of freed
   * memory. Reading every cell here rather than the row count, since a
   * count is a field on the result and the rows are what would be
   * borrowed. */
  zu_conn *conn = NULL;
  zu_result *res = NULL;
  const char *name = NULL;
  size_t len = 0;
  uint64_t i = 0;

  ZT_CHECK_EQ(zu_memory(&conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(conn, "UNWIND ['ada', 'grace', 'alan'] AS name RETURN name", &res,
                         NULL),
              ZU_OK);
  zu_conn_close(conn);

  ZT_CHECK_EQ(zu_result_rows(res), 3);
  ZT_CHECK_EQ(zu_result_cols(res), 1);
  ZT_CHECK_EQ(zu_result_col_name(res, 0, &name, &len), ZU_OK);
  ZT_CHECK_STR(name, len, "name");
  for (i = 0; i < 3; i++) {
    ZT_CHECK_EQ(zu_result_cell_str(res, i, 0, &name, &len), ZU_OK);
    ZT_CHECK(len > 0);
  }
  zu_result_free(res);
}

ZT_TEST(a_cell_out_of_range_is_misuse_rather_than_a_read_of_somebody_elses_memory) {
  /* An index arithmetic slip is how a C host walks off the end of a
   * result, and off the end of a result is somebody else's memory. Both
   * ends of both axes, and the row after the last, which is the one an
   * off-by-one actually produces. */
  zu_conn *conn = NULL;
  zu_result *res = NULL;
  const char *cell = NULL;
  size_t len = 0;
  int32_t type = 0;

  ZT_CHECK_EQ(zu_memory(&conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(conn, "RETURN 'ada' AS name", &res, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_result_rows(res), 1);

  ZT_CHECK_EQ(zu_result_cell_str(res, 1, 0, &cell, &len), ZU_MISUSE);
  ZT_CHECK_EQ(zu_result_cell_str(res, 0, 1, &cell, &len), ZU_MISUSE);
  ZT_CHECK_EQ(zu_result_cell_str(res, UINT64_MAX, 0, &cell, &len), ZU_MISUSE);
  ZT_CHECK_EQ(zu_result_cell_str(res, 0, UINT32_MAX, &cell, &len), ZU_MISUSE);
  ZT_CHECK_EQ(zu_result_col_name(res, 1, &cell, &len), ZU_MISUSE);

  /* The type of a cell out of range is the documented -1 rather than a
   * status, because it returns the tag directly. */
  ZT_CHECK_EQ(zu_result_cell_type(res, 1, 0, &type), ZU_MISUSE);

  zu_result_free(res);
  zu_conn_close(conn);
}

ZT_TEST(a_thousand_connections_opened_and_closed_leave_no_descriptors_behind) {
  /* The leak that does not look like a leak. A connection carries a
   * file handle, and a host that opens one per request against a
   * process with a soft limit of a thousand and twenty four falls over
   * on the thousandth request rather than on the first, hours in, with
   * a message about too many open files and nothing pointing here.
   *
   * Measured as the lowest free descriptor rather than as a count of
   * anything, so it holds on both platforms this runs on. */
  zt_tmpdir dir;
  char path[640];
  int before = 0;
  int after = 0;
  int i = 0;

  ZT_CHECK(zt_tmpdir_open(&dir, "conns"));
  zt_tmpdir_file(&dir, "people.zu", path, sizeof path);
  if (!people(path)) {
    zt_tmpdir_close(&dir);
    return;
  }

  /* One first, so that whatever the library opens once and keeps is
   * already open when the mark is taken. */
  {
    zu_conn *warm = NULL;
    ZT_CHECK_EQ(zu_open_z(path, &warm, NULL), ZU_OK);
    zu_conn_close(warm);
  }

  before = zt_next_fd();
  for (i = 0; i < 1000; i++) {
    zu_conn *conn = NULL;
    if (zu_open_z(path, &conn, NULL) != ZU_OK) {
      zt_report(__FILE__, __LINE__, "open %d of a thousand failed", i);
      break;
    }
    zu_conn_close(conn);
  }
  after = zt_next_fd();
  zt_tmpdir_close(&dir);

  ZT_CHECK(before > 0);
  ZT_CHECK_EQ(after, before);
}

ZT_TEST(an_open_that_will_not_work_says_which_kind_of_wrong_it_was) {
  /* Before counting descriptors on the failure path, what the failure
   * path answers, because the two kinds are not the same problem and a
   * host has to tell them apart to say anything useful to a user.
   *
   * A path with nothing at it and a path that is a directory are the
   * operating system declining, which is ZU_IO. A file that is there
   * and is not a zu1 database is the file being wrong, which is
   * ZU_CORRUPT. Neither is ZU_ERROR: nothing was refused by the engine
   * because nothing got as far as the engine. */
  zt_tmpdir dir;
  char path[640];
  char missing[640];
  zu_conn *conn = NULL;
  FILE *f = NULL;

  ZT_CHECK(zt_tmpdir_open(&dir, "openkind"));
  zt_tmpdir_file(&dir, "notadb.zu", path, sizeof path);
  zt_tmpdir_file(&dir, "nothinghere.zu", missing, sizeof missing);
  f = fopen(path, "wb");
  if (f == NULL) {
    zt_tmpdir_close(&dir);
    ZT_FAIL("could not write %s", path);
  }
  fputs("this is not a database", f);
  fclose(f);

  ZT_CHECK_EQ(zu_open_z(path, &conn, NULL), ZU_CORRUPT);
  ZT_CHECK(conn == NULL);
  ZT_CHECK_EQ(zu_open_z(missing, &conn, NULL), ZU_IO);
  ZT_CHECK(conn == NULL);
  ZT_CHECK_EQ(zu_open_z(dir.path, &conn, NULL), ZU_IO);
  ZT_CHECK(conn == NULL);

  zt_tmpdir_close(&dir);
}

ZT_TEST(five_hundred_opens_that_failed_leave_no_descriptors_behind) {
  /* The same leak as the case above it, on the path nobody tests,
   * which is the path that runs when a user points a program at the
   * wrong file. An open that gets far enough to read a header and then
   * rejects what it read has a descriptor to give back, and the
   * failure path is where giving it back gets forgotten.
   *
   * Both kinds, because they fail at different depths: the corrupt one
   * opened the file and read from it, the missing one never opened
   * anything, and only the first has a descriptor to lose. */
  zt_tmpdir dir;
  char path[640];
  char missing[640];
  FILE *f = NULL;
  int before = 0;
  int after = 0;
  int i = 0;

  ZT_CHECK(zt_tmpdir_open(&dir, "failopen"));
  zt_tmpdir_file(&dir, "notadb.zu", path, sizeof path);
  zt_tmpdir_file(&dir, "nothinghere.zu", missing, sizeof missing);
  f = fopen(path, "wb");
  if (f == NULL) {
    zt_tmpdir_close(&dir);
    ZT_FAIL("could not write %s", path);
  }
  fputs("this is not a database", f);
  fclose(f);

  /* One of each first, so that whatever the library sets up once on a
   * failure is already set up when the mark is taken. */
  {
    zu_conn *conn = NULL;
    ZT_CHECK_EQ(zu_open_z(path, &conn, NULL), ZU_CORRUPT);
    ZT_CHECK_EQ(zu_open_z(missing, &conn, NULL), ZU_IO);
  }

  before = zt_next_fd();
  for (i = 0; i < 500; i++) {
    zu_conn *conn = NULL;
    zu_error *err = NULL;
    const char *which = (i % 2) == 0 ? path : missing;
    if (zu_open_z(which, &conn, &err) == ZU_OK) {
      zu_conn_close(conn);
      zt_report(__FILE__, __LINE__, "an open written to fail worked instead");
      break;
    }
    zu_error_free(err);
  }
  after = zt_next_fd();
  zt_tmpdir_close(&dir);

  ZT_CHECK(before > 0);
  ZT_CHECK_EQ(after, before);
}

ZT_TEST(a_connection_closed_inside_a_transaction_leaves_the_database_as_it_was) {
  /* Abandoning a transaction by closing rather than ending it is what
   * an early return in C looks like from the outside. The rule it has
   * to obey is the one a user assumes without checking: nothing that
   * was not committed is there afterwards.
   *
   * Checked by reopening rather than by asking the connection, because
   * asking the connection that did the writing is asking the cache. */
  zt_tmpdir dir;
  char path[640];
  zu_conn *conn = NULL;
  zu_result *res = NULL;
  int32_t in_txn = 0;
  int64_t before = 0;
  int64_t after = 0;
  const zu_value *cell = NULL;

  ZT_CHECK(zt_tmpdir_open(&dir, "abandon"));
  zt_tmpdir_file(&dir, "people.zu", path, sizeof path);
  if (!people(path)) {
    zt_tmpdir_close(&dir);
    return;
  }

  ZT_CHECK_EQ(zu_open_z(path, &conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(conn, "MATCH (p:Person) RETURN count(*) AS n", &res, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_result_cell(res, 0, 0, &cell), ZU_OK);
  ZT_CHECK_EQ(zu_value_i64(cell, &before), ZU_OK);
  zu_result_free(res);
  res = NULL;

  ZT_CHECK_EQ(zu_begin(conn, 0, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_conn_in_transaction(conn, &in_txn), ZU_OK);
  ZT_CHECK_EQ(in_txn, 1);
  ZT_CHECK_EQ(zu_query_z(conn, "INSERT (:Person {id: 4, name: 'edsger'})", &res, NULL), ZU_OK);
  zu_result_free(res);
  res = NULL;

  /* No commit and no rollback. Just gone, the way an early return
   * leaves it. */
  zu_conn_close(conn);
  conn = NULL;

  ZT_CHECK_EQ(zu_open_z(path, &conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(conn, "MATCH (p:Person) RETURN count(*) AS n", &res, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_result_cell(res, 0, 0, &cell), ZU_OK);
  ZT_CHECK_EQ(zu_value_i64(cell, &after), ZU_OK);
  zu_result_free(res);
  zu_conn_close(conn);
  zt_tmpdir_close(&dir);

  ZT_CHECK_EQ(after, before);
}

ZT_TEST(a_statement_that_failed_left_the_connection_fit_to_use) {
  /* A refusal is not damage. This is the case that says a host may
   * keep the connection after a bad statement rather than throwing it
   * away, which is the difference between a REPL and a REPL that
   * reconnects after every typo.
   *
   * Every kind of failure the connection can see, one after another on
   * the one connection, then a statement that has to work. */
  zu_conn *conn = NULL;
  zu_result *res = NULL;
  const zu_value *cell = NULL;
  int64_t n = 0;

  ZT_CHECK_EQ(zu_memory(&conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(conn, "MATCH (", &res, NULL), ZU_ERROR);
  ZT_CHECK_EQ(zu_query_z(conn, "RETURN nope", &res, NULL), ZU_ERROR);
  ZT_CHECK_EQ(zu_query_z(conn, "RETURN 1 / 0", &res, NULL), ZU_ERROR);
  ZT_CHECK_EQ(zu_query_z(conn, "RETURN 1 + 'ada'", &res, NULL), ZU_ERROR);
  ZT_CHECK_EQ(zu_query_z(conn, "", &res, NULL), ZU_ERROR);

  ZT_CHECK_EQ(zu_query_z(conn, "RETURN 41 + 1 AS n", &res, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_result_cell(res, 0, 0, &cell), ZU_OK);
  ZT_CHECK_EQ(zu_value_i64(cell, &n), ZU_OK);
  ZT_CHECK_EQ(n, 42);
  zu_result_free(res);

  /* And it never quietly opened a transaction on the way. */
  {
    int32_t in_txn = 1;
    ZT_CHECK_EQ(zu_conn_in_transaction(conn, &in_txn), ZU_OK);
    ZT_CHECK_EQ(in_txn, 0);
  }
  zu_conn_close(conn);
}

/* The rendezvous for the case below. The watcher runs on a thread of
 * the library's while the statement runs, which makes it the one place
 * a second thread is guaranteed to be inside a call on this connection
 * at a known moment. */
struct reentry {
  zu_conn *conn;
  int calls;
  zu_status query;
  zu_status prepare;
  zu_status begin;
  zu_status rows_read;
  zu_status interrupt;
};

static int call_back_in(void *user_data, uint64_t rows, uint64_t ms) {
  struct reentry *state = (struct reentry *)user_data;
  (void)rows;
  (void)ms;
  if (state->calls == 0) {
    /* Exactly what the header says not to do: "a callback must not
     * call back into this library on the connection it is reporting
     * on". The point of the case is that being told not to is backed
     * by an answer rather than by luck.
     *
     * The last two are the calls that are allowed from here, taken in
     * the same breath so that the case says where the line is rather
     * than only that there is one. Interrupt is not called, because
     * calling it would stop the statement and the case would then be
     * about interruption. */
    zu_result *res = NULL;
    zu_stmt *stmt = NULL;
    uint64_t read = 0;
    state->query = zu_query_z(state->conn, "RETURN 1 AS one", &res, NULL);
    zu_result_free(res);
    state->prepare = zu_prepare_z(state->conn, "RETURN 1 AS one", &stmt, NULL);
    zu_stmt_close(stmt);
    state->begin = zu_begin(state->conn, 0, NULL);
    state->rows_read = zu_conn_rows_read(state->conn, &read);
  }
  state->calls++;
  return 1;
}

ZT_TEST(a_call_back_into_the_library_from_the_watcher_is_refused_rather_than_raced) {
  /* ZU_MISUSE_CONCURRENT is one of the three headline promises of this
   * ABI and nothing was checking it. It is also the hardest of the
   * three to check, because two threads racing is not a test: whichever
   * way it comes out, it came out that way once.
   *
   * The progress watcher makes it deterministic. It runs on a thread of
   * the library's, and it only runs while a statement is running, so a
   * call made from inside it is a call made from a second thread at a
   * moment when the first is certainly inside the executor. */
  static int64_t ids[3000];
  const uint64_t rows = 3000;
  zu_database *db = NULL;
  zu_conn *conn = NULL;
  zu_frame *frame = NULL;
  zu_result *res = NULL;
  struct reentry state;
  uint64_t i = 0;

  for (i = 0; i < rows; i++) {
    ids[i] = (int64_t)i;
  }
  memset(&state, 0, sizeof state);

  ZT_CHECK_EQ(zu_database_memory(NULL, &db, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_connect(db, &conn, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_frame_new_z("Person", rows, NULL, NULL, &frame, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_frame_col_int(frame, "id", 2, ids, rows, 64, 1, 1, ZU_FRAME_PLAIN, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_conn_register(conn, frame, NULL), ZU_OK);

  state.conn = conn;
  ZT_CHECK_EQ(zu_conn_set_progress(conn, call_back_in, &state, 1), ZU_OK);

  /* A pair of patterns with a predicate over them, which the planner
   * cannot fold into a count, so it runs long enough to be reported on.
   * The same statement test_progress.cpp uses, for the same reason. */
  ZT_CHECK_EQ(zu_query_z(conn, "MATCH (a:Person), (b:Person) WHERE a.id < b.id RETURN count(*)",
                         &res, NULL),
              ZU_OK);
  zu_result_free(res);

  ZT_CHECK(state.calls > 0);
  ZT_CHECK_EQ(state.query, ZU_MISUSE_CONCURRENT);
  ZT_CHECK_EQ(state.prepare, ZU_MISUSE_CONCURRENT);
  ZT_CHECK_EQ(state.begin, ZU_MISUSE_CONCURRENT);

  /* And the watch itself is allowed from the watcher, which is the
   * half that makes the rule usable: a progress callback that wants to
   * report a rate needs the row count, and the header names only
   * zu_conn_interrupt as the call that may cross threads. Asserted
   * because it is real behaviour a host will lean on, and filed
   * against the header, which does not say so. */
  ZT_CHECK_EQ(state.rows_read, ZU_OK);

  ZT_CHECK_EQ(zu_conn_set_progress(conn, NULL, NULL, 0), ZU_OK);
  zu_conn_close(conn);
  zu_frame_free(frame);
  zu_database_close(db);
}

ZT_TEST(the_programs_that_look_like_misuse_and_are_not) {
  /* The other half of this item, and the half that decides whether a
   * strict library is usable. Each of these looks wrong at a glance and
   * every one of them is written down as allowed, so a change that
   * started rejecting any of them would break working programs while
   * looking like a tightening. */
  zu_database *db = NULL;
  zu_conn *first = NULL;
  zu_conn *second = NULL;
  zu_result *res = NULL;
  int32_t removed = 0;
  uint64_t count = 1;

  /* A null pointer with a zero length is the empty string and not an
   * error, which is what a host whose strings are counted passes for
   * one. Empty is not a statement, so the query is refused; the point
   * is that it is refused as a statement rather than as a bad pointer. */
  ZT_CHECK_EQ(zu_memory(&first, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_query(first, NULL, 0, &res, NULL), ZU_ERROR);
  ZT_CHECK(res == NULL);

  /* Closing a database before the connections opened from it, which
   * reads like closing a file while it is being written and is written
   * down as fine: each connection holds its own handle and the database
   * is only the path and the configuration. */
  ZT_CHECK_EQ(zu_database_memory(NULL, &db, NULL), ZU_OK);
  ZT_CHECK_EQ(zu_connect(db, &second, NULL), ZU_OK);
  zu_database_close(db);
  ZT_CHECK_EQ(zu_query_z(second, "RETURN 1 AS one", &res, NULL), ZU_OK);
  zu_result_free(res);

  /* Unregistering a frame that was never registered, which a cleanup
   * path does when it does not track what it registered. It says it
   * removed nothing rather than failing. */
  ZT_CHECK_EQ(zu_conn_unregister_z(second, "Nobody", &removed, NULL), ZU_OK);
  ZT_CHECK_EQ(removed, 0);
  ZT_CHECK_EQ(zu_conn_registered_count(second, &count), ZU_OK);
  ZT_CHECK_EQ(count, 0);

  /* Interrupting a connection that is not running anything, which is
   * what a Ctrl-C at an idle prompt is. Dropped when the next statement
   * starts rather than aimed at it. */
  ZT_CHECK_EQ(zu_conn_interrupt(second), ZU_OK);
  ZT_CHECK_EQ(zu_query_z(second, "RETURN 1 AS one", &res, NULL), ZU_OK);
  zu_result_free(res);

  /* The old spelling of close, which the header keeps until the
   * freeze. */
  zu_close(second);
  zu_conn_close(first);
}

ZT_MAIN(ZT_CASE(a_wrong_program_is_told_what_is_wrong),
        ZT_CASE(a_call_that_failed_wrote_its_out_parameter_rather_than_leaving_it),
        ZT_CASE(an_error_nobody_asked_for_is_dropped_and_the_status_still_says),
        ZT_CASE(a_null_handle_is_misuse_rather_than_a_crash),
        ZT_CASE(giving_back_nothing_is_allowed_everywhere_it_is_written_down),
        ZT_CASE(a_statement_used_after_its_connection_closed_says_so_rather_than_following_a_pointer),
        ZT_CASE(a_result_outlives_its_connection_because_it_owns_its_rows),
        ZT_CASE(a_cell_out_of_range_is_misuse_rather_than_a_read_of_somebody_elses_memory),
        ZT_CASE(a_thousand_connections_opened_and_closed_leave_no_descriptors_behind),
        ZT_CASE(an_open_that_will_not_work_says_which_kind_of_wrong_it_was),
        ZT_CASE(five_hundred_opens_that_failed_leave_no_descriptors_behind),
        ZT_CASE(a_connection_closed_inside_a_transaction_leaves_the_database_as_it_was),
        ZT_CASE(a_statement_that_failed_left_the_connection_fit_to_use),
        ZT_CASE(a_call_back_into_the_library_from_the_watcher_is_refused_rather_than_raced),
        ZT_CASE(the_programs_that_look_like_misuse_and_are_not))

/* The C ABI on its own, with no C++ anywhere near it.
 *
 * This is the shape every other client sits on: a status out of every
 * call, an out-parameter for what it made, and a zu_error carrying the
 * GQLSTATUS when the status is not ZU_OK. Nothing here is clever. The
 * point of the C example is that the plain reading is the right one. */
#include <zu.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void complain(const char *what, zu_error *err) {
  size_t code_len = 0;
  size_t msg_len = 0;
  const char *code = err == NULL ? NULL : zu_error_code(err, &code_len);
  const char *msg = err == NULL ? NULL : zu_error_message(err, &msg_len);
  fprintf(stderr, "%s: %.*s %.*s\n", what, (int)code_len, code == NULL ? "" : code,
          (int)msg_len, msg == NULL ? "" : msg);
  zu_error_free(err);
}

int main(void) {
  zu_conn *conn = NULL;
  zu_result *res = NULL;
  zu_error *err = NULL;

  /* A database with no file behind it, which is the shortest way to a
   * connection and the one an example wants. zu_open takes a path and
   * its length for a host whose strings are not NUL-terminated; the _z
   * spellings take a C string when they are. */
  if (zu_memory(&conn, &err) != ZU_OK) {
    complain("zu_memory", err);
    return 1;
  }

  const char *q = "UNWIND ['ada', 'grace', 'alan'] AS name RETURN name";
  if (zu_query(conn, q, strlen(q), &res, &err) != ZU_OK) {
    complain("zu_query", err);
    zu_conn_close(conn);
    return 1;
  }

  printf("%" PRIu64 " rows over %" PRIu32 " column\n", zu_result_rows(res),
         zu_result_cols(res));
  for (uint64_t i = 0; i < zu_result_rows(res); i++) {
    const char *name = NULL;
    size_t len = 0;
    /* The pointer is into the result's own bytes and is good until
     * zu_result_free, so nothing is copied to read a row. */
    if (zu_result_cell_str(res, i, 0, &name, &len) != ZU_OK) {
      fprintf(stderr, "cell %" PRIu64 " is not a string\n", i);
      continue;
    }
    printf("  %.*s\n", (int)len, name);
  }

  zu_result_free(res);
  zu_conn_close(conn);
  return 0;
}

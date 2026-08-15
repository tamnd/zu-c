# zu for C and C++

The C and C++ developer kit for [zu](https://github.com/tamnd/zu), an embedded property-graph database.

**This repository does not contain `zu.h`.** The header is generated from the engine's API model and ships from [tamnd/zu](https://github.com/tamnd/zu) with `libzu`, because a header generated somewhere else is a header that drifts. What lives here is everything around it: examples, the header-only C++ wrapper, the packaging for every build system C and C++ people actually use, and the sanitizer suites that keep the ABI honest.

The C ABI is tier 1 and it is load-bearing. Every other client, Python, Node, Go, Java, .NET, and every tier-3 binding, sits on it.

```c
#include <zu.h>
#include <stdio.h>

int main(void) {
    zu_database *db = NULL;
    zu_conn *conn = NULL;
    zu_result *res = NULL;
    zu_error *err = NULL;

    if (zu_database_open("social.zu1", NULL, &db, &err) != ZU_OK) goto fail;
    if (zu_connect(db, &conn, &err) != ZU_OK) goto fail;
    if (zu_query(conn, "MATCH (p:Person) RETURN p.name AS name LIMIT 5", &res, &err) != ZU_OK) goto fail;

    for (uint64_t i = 0; i < zu_result_rows(res); i++) {
        size_t len;
        const char *name = zu_result_cell_str(res, i, 0, &len);
        printf("%.*s\n", (int)len, name);
    }

    zu_result_free(res);
    zu_conn_close(conn);
    zu_database_close(db);
    return 0;

fail:
    fprintf(stderr, "%s: %s\n", zu_error_code(err), zu_error_message(err));
    zu_error_free(err);
    zu_result_free(res); zu_conn_close(conn); zu_database_close(db);
    return 1;
}
```

The same thing in C++, where the wrapper does the cleanup:

```cpp
#include <zu.hpp>

int main() {
    auto db = zu::Database::open("social.zu1");
    auto conn = db.connect();
    for (auto row : conn.query("MATCH (p:Person) RETURN p.name AS name LIMIT 5"))
        std::println("{}", row.get<std::string_view>("name"));
}
```

## What is here

- `examples/`, every example compiles and runs in CI, on every supported platform, under ASan and UBSan.
- `include/zu.hpp`, the header-only C++20 wrapper. RAII on every handle, exceptions carrying the GQLSTATUS condition, ranges over results, `std::span` over columns. Optional and additive, the C API stays usable on its own.
- `cmake/`, `vcpkg/`, `conan/`, `pkgconfig/`, find it the way your project already finds things. `find_package(zu)`, `vcpkg install zu`, `conan install zu`, or plain `pkg-config --cflags --libs zu`.
- `sanitizers/`, ASan, UBSan, TSan, and Valgrind suites over the full ABI surface, including the deliberate misuse cases, because an ABI nine languages depend on should fail loudly rather than corrupt quietly.

## Specification

Spec/2064g/dx/09-c-cpp.md and dx/02-c-abi.md in [tamnd/zu](https://github.com/tamnd/zu). Milestone: DX1 (tamnd/zu#167).

## Status

Pre-1.0 and pre-release. Nothing is published yet. The engine, the C ABI, and this client all move on one version number, so a release here always pairs with the same release of [`tamnd/zu`](https://github.com/tamnd/zu).

## Where things live

| What | Where |
|---|---|
| Engine, Rust SDK, CLI, `zu.h`, conformance corpus | [tamnd/zu](https://github.com/tamnd/zu) |
| Documentation and website | [tamnd/zu-web](https://github.com/tamnd/zu-web) |
| This client | here |

If a bug reproduces through the `zu` CLI, it belongs in [tamnd/zu](https://github.com/tamnd/zu/issues), not here.

## License

Apache-2.0, same as the engine.

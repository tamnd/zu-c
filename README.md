# zu for C and C++

The C and C++ developer kit for [zu](https://github.com/tamnd/zu), an embedded property-graph database.

**This repository does not contain `zu.h`.** The header is generated from the engine's API model and ships from [tamnd/zu](https://github.com/tamnd/zu) with `libzu`, because a header generated somewhere else is a header that drifts. What lives here is everything around it: examples, the header-only C++ wrapper, the packaging for every build system C and C++ people actually use, and the sanitizer suites that keep the ABI honest.

The C ABI is tier 1 and it is load-bearing. Every other client, Python, Node, Go, Java, .NET, and every tier-3 binding, sits on it.

```c
#include <zu.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char *path = "social.zu1";
    const char *people[] = {
        "INSERT (p:Person {id: 1, name: 'ada'})",
        "INSERT (p:Person {id: 2, name: 'grace'})",
        "INSERT (p:Person {id: 3, name: 'lynn'})",
    };
    const char *q = "MATCH (p:Person) RETURN p.name AS name ORDER BY p.id";
    zu_conn *conn = NULL;
    zu_result *res = NULL;
    zu_error *err = NULL;

    /* zu_create is a database and one connection on it, which is what a
       program with one thread wants. zu_open is the call for a database
       that is already there, and they are two calls rather than one flag
       so that a deployment pointed at the wrong path is told about it
       instead of quietly starting from empty. Every call takes a pointer
       and a length, so a host whose strings are not NUL terminated never
       has to copy one; the _z spellings take a C string when it is. */
    if (zu_create(path, strlen(path), &conn, &err) != ZU_OK) goto fail;

    for (size_t i = 0; i < sizeof people / sizeof people[0]; i++) {
        if (zu_query(conn, people[i], strlen(people[i]), &res, &err) != ZU_OK) goto fail;
        zu_result_free(res);
        res = NULL;
    }

    if (zu_query(conn, q, strlen(q), &res, &err) != ZU_OK) goto fail;

    for (uint64_t i = 0; i < zu_result_rows(res); i++) {
        const char *name = NULL;
        size_t len = 0;
        if (zu_result_cell_str(res, i, 0, &name, &len) != ZU_OK) continue;
        printf("%.*s\n", (int)len, name);
    }

    zu_result_free(res);
    zu_conn_close(conn);
    return 0;

fail: {
    size_t code_len = 0, msg_len = 0;
    const char *code = zu_error_code(err, &code_len);
    const char *msg = zu_error_message(err, &msg_len);
    fprintf(stderr, "%.*s: %.*s\n", (int)code_len, code, (int)msg_len, msg);
    zu_error_free(err);
    zu_result_free(res);
    zu_conn_close(conn);
    return 1;
}
}
```

```
ada
grace
lynn
```

That output is not a promise, it is a test. Both programs on this page are compiled and run by this repository's suite exactly as printed, and what they print is diffed against the block under them, because a first example is the most read and least executed code a client has and it goes wrong quietly, a rename at a time. Both write `social.zu1` in the directory you run them from, and running one twice fails on the second go: `zu_create` refuses a path that is already there.

The same thing in C++, where the wrapper does the cleanup:

```cpp
#include <zu.hpp>
#include <iostream>

int main() {
    auto conn = zu::Connection::create("social.zu1");
    conn.query("INSERT (p:Person {id: 1, name: 'ada'})");
    conn.query("INSERT (p:Person {id: 2, name: 'grace'})");
    conn.query("INSERT (p:Person {id: 3, name: 'lynn'})");

    auto rows = conn.query("MATCH (p:Person) RETURN p.name AS name ORDER BY p.id");
    for (auto row : rows)
        std::cout << row.get<std::string_view>("name") << '\n';
}
```

```
ada
grace
lynn
```

`rows` is a named variable rather than a temporary on purpose. A `std::string_view` read out of a result points into the result's own bytes, and a result that died at the end of the statement has nothing left to point into. That is the one rule the zero copy path asks you to keep.

## What is here

- `include/zu.hpp`, the header-only C++20 wrapper. RAII on every handle, exceptions carrying the GQLSTATUS condition, ranges over results, `std::span` over columns, `std::formatter` on everything worth printing, and a `std::expected` mirror of the whole error model under C++23. Optional and additive, the C API stays usable on its own.
- `test/`, the suite. Every case is built twice, once at C++23 and once at the C++20 floor, so the standard the header claims to support is the standard it is tested against. `test/test_idiom.cpp` is mostly `static_assert`, and deliberately: that a `Result` is a random access range, that a handle moves and refuses to be copied, that a view into a result is not a borrowed one, and that an exception is a `std::exception` are promises the compiler should keep at every call site rather than ones a case checked once.
- `examples/`, one per thing worth knowing. Every example is also a test, because an example that compiles and does not run is documentation that lies.
- `readme/`, which lifts the two programs above off this page, builds them, runs them and diffs what they print against the blocks under them. The page is the code most people read and the code least often run, and it is the only code here that had nothing compiling it.
- `bench/`, the numbers below, with a timing harness that needs no package manager to run.
- `cmake/`, `find_package(Zu)` to find the engine and `find_package(zu-cpp)` to find this. vcpkg, Conan and pkg-config packaging come with the first release.

Four sanitizer jobs run over the suite, because an ABI nine languages depend on should fail loudly rather than corrupt quietly. The whole tree runs under ASan and UBSan; `test/misuse.c` runs again with leak detection on, which it can and the C++ files cannot, because it is the file that gives every handle back by hand; `test/threads.c` runs under TSan; and both C files run under valgrind, which sees what the sanitizers cannot, since libzu is compiled without instrumentation and memcheck does not need any. `test/tsan.supp` records what TSan is unable to be told about a library that takes no pthread lock, and why the reports from inside the engine are dropped rather than read.

## Building

The engine is a separate repository and is not built here. Point at a checkout of it, or at an installed SDK, and everything else follows.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release -DZU_ROOT=/path/to/zu
cmake --build build
ctest --test-dir build --output-on-failure
```

A checkout with no engine beside it still configures and installs the header, and skips the suite, because a header-only library compiles against a header. `cmake --build build --target bench` builds the benchmarks, which ctest does not run: a timing number produced on a machine that is also running a compile is not a number.

The wrapper is header-only, so a project that would rather not use CMake needs the include path and nothing else.

## Numbers

One core of an M-series laptop, release build, a million rows registered as a frame. Rounded, and reproducible with `cmake --build build --target bench && ./build/bench/bench_read`.

| What | Per call | Rows per second |
|---|---|---|
| Column read as a `std::span`, summed | 198 us | 5.0 billion |
| The same sum over a plain `std::vector` | 212 us | 4.7 billion |
| The same million rows one row at a time | 17.4 ms | 58 million |
| Registering a frame of a thousand rows | 1.9 us | |
| Registering a frame of a million rows | 2.2 us | |
| Scan a million borrowed rows and count | 275 us | 3.6 billion |
| One small statement, start to finish | 3.4 us | |

Two of those rows are the whole design. A column read as a span is as fast as summing the `std::vector` it was borrowed from, give or take the noise, because it is the same memory and nothing was copied to hand it over. And registering a million rows costs the same as registering a thousand, for the same reason: what crosses the boundary is a pointer and a length.

The row-at-a-time spellings are eighty times slower and they are not a mistake. They go through a bounds check and a type tag per cell, which is what it costs to read a column whose type you do not know until runtime. Reach for them when that is the situation and for the spans when it is not.

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

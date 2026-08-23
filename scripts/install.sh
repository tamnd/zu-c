#!/bin/sh
#
# The install, run inside the container the Install workflow starts.
#
# It is a program rather than lines in a YAML file because it is run four
# times, once for the install and three times to prove the install would
# have failed if a piece of it were missing, and because a thing that
# decides whether a client is installable should be readable without
# opening a workflow.
#
# What it does is what a reader does. Install the wrapper into a prefix,
# take the two whole programs off the README, put each in an empty
# directory somewhere else on the machine, build them against the prefix
# and run them, and diff what they print against the block under them on
# the page. Nothing is copied out of the checkout except those two
# programs and the install, and the checkout is mounted read only so
# that a build cannot reach back into it for a header.
#
# It is driven by these, all set by the workflow:
#
#   SRC     the checkout, read only
#   SDK     an engine SDK, read only: include/zu.h and lib/libzu.so,
#           which is what a release archive unpacks to. There is no
#           release yet, so the workflow builds one with cargo and lays
#           it out, the same way it would arrive.
#   APP     an empty directory, outside the checkout, to work in
#   ABSENT  the tools this image is claimed not to have
#   MODE    cmake, for a project that runs find_package(zu-cpp), or
#           header, for one that wants the include path and nothing else
#   BREAK   what to take out of the install before building, for the runs
#           that have to fail. See the bottom of this file.

set -eu

: "${SRC:=/src}"
: "${SDK:=/sdk}"
: "${APP:=/app}"
: "${ABSENT:=rustc cargo}"
: "${MODE:=cmake}"
: "${BREAK:=}"

prefix="$APP/prefix"

# What the image is claimed to be, checked rather than believed. The day
# a base image starts shipping a Rust toolchain is the day this job
# quietly stops being about anything, because the thing it exists to
# prove is that none of it is needed.
for tool in $ABSENT; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "this image has $tool on it, so it is not the machine this job is about" >&2
        exit 1
    fi
done

# And no zu already on it, which is the failure that would be worst of
# all: the install would look like it worked while the compiler took a
# header and the linker took a library that a previous job left behind.
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists zu 2>/dev/null; then
    echo "this image has a libzu registered with pkg-config" >&2
    exit 1
fi
for path in /usr/include/zu.h /usr/local/include/zu.h \
    /usr/include/zu.hpp /usr/local/include/zu.hpp \
    /usr/lib/libzu.so /usr/local/lib/libzu.so; do
    if [ -e "$path" ]; then
        echo "this image has $path on it, so the install is not what would be used" >&2
        exit 1
    fi
done

# The SDK is the engine's half and it has to be there, since neither
# zu.h nor libzu is in this repository and a job that ran without them
# would be reporting on a compile that never linked.
test -f "$SDK/include/zu.h" || { echo "no zu.h in $SDK" >&2; exit 1; }
# The glob expands to itself when it matches nothing, so this is asking
# whether there is a library rather than whether the shell was in the
# mood to say so.
set -- "$SDK"/lib/libzu.*
test -e "$1" || { echo "no libzu in $SDK/lib" >&2; exit 1; }

# ---- install ----

mkdir -p "$APP"

case "$MODE" in
cmake)
    # The install a user runs, from the checkout, with the suite and the
    # examples off because a user installing a header is not building
    # this repository's tests.
    cmake --version | head -1
    cmake -S "$SRC" -B "$APP/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DZU_CPP_TESTS=OFF -DZU_CPP_EXAMPLES=OFF -DZU_CPP_BENCH=OFF \
        -DZU_ROOT="$SDK" \
        -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null
    cmake --build "$APP/build" --target install >/dev/null
    ;;
header)
    # The other claim the page makes: the wrapper is header only, so a
    # project that would rather not use CMake needs the include path and
    # nothing else. That is checked by doing it, on an image with no
    # CMake on it at all, rather than by saying so.
    mkdir -p "$prefix/include"
    cp "$SRC/include/zu.hpp" "$prefix/include/zu.hpp"
    ;;
*)
    echo "MODE is cmake or header, not $MODE" >&2
    exit 1
    ;;
esac

# The run that has to fail, set up. Everything above this line is the
# install; what is taken out here is taken out of the install rather
# than out of the source, so that a build which still succeeds is a
# build that found the missing piece somewhere else on the machine.
case "$BREAK" in
"") ;;
header)
    rm -f "$prefix/include/zu.hpp"
    ;;
package)
    # By searching rather than by naming a path, because GNUInstallDirs
    # picks the library directory and it is not lib everywhere.
    find "$prefix" -type d -name zu-cpp -exec rm -rf {} + 2>/dev/null || true
    ;;
sdk)
    # Not the install: the engine beside it. A wrapper that compiled
    # without libzu would be a wrapper that is not calling it.
    SDK="$APP/no-sdk"
    mkdir -p "$SDK"
    ;;
*)
    echo "BREAK is header, package, sdk or empty, not $BREAK" >&2
    exit 1
    ;;
esac

# ---- the two programs on the page ----

# Taken off the page rather than written again here, so that what this
# job installs is what a reader copies. One fenced block per language
# and the first one wins, which is the same rule readme/CMakeLists.txt
# applies when it builds them as part of the suite.
lift() {
    awk -v fence="\`\`\`$1" '
        $0 == fence            { inside = 1; n = 0; next }
        $0 == "```" && inside  { for (i = 1; i <= n; i++) print line[i]; exit }
        inside                 { n++; line[n] = $0 }
    ' "$SRC/README.md"
}

want='ada
grace
lynn'

# Built into one place and run from another, each in a directory of its
# own, because both programs write social.zu1 beside themselves and
# zu_create refuses a path that is already there.
mkdir -p "$APP/bin"

run() {
    name="$1"
    where="$APP/work-$name"
    rm -rf "$where"
    mkdir -p "$where"
    got=$(cd "$where" && "$APP/bin/$name")
    if [ "$got" != "$want" ]; then
        echo "the $name quickstart printed:" >&2
        echo "$got" >&2
        echo "and the page says it prints:" >&2
        echo "$want" >&2
        exit 1
    fi
}

libdir="$SDK/lib"

# The C++ one, which is what this client installs.
mkdir -p "$APP/src-cpp"
lift cpp > "$APP/src-cpp/main.cpp"
test -s "$APP/src-cpp/main.cpp" || {
    echo "the README has no C++ program on it, which is a page to fix rather than an install to report" >&2
    exit 1
}

if [ "$MODE" = cmake ]; then
    cat > "$APP/src-cpp/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(quickstart LANGUAGES CXX)
find_package(zu-cpp REQUIRED)
add_executable(cpp main.cpp)
target_link_libraries(cpp PRIVATE zu::cpp)
EOF
    cmake -S "$APP/src-cpp" -B "$APP/build-cpp" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$prefix" \
        -DZU_ROOT="$SDK"
    cmake --build "$APP/build-cpp"
    cp "$APP/build-cpp/cpp" "$APP/bin/cpp"
else
    # The include path and one -l, which is the whole of what the page
    # claims a project without CMake needs.
    ${CXX:-c++} -std=c++20 -O2 \
        -I"$prefix/include" -I"$SDK/include" \
        "$APP/src-cpp/main.cpp" \
        -L"$libdir" -lzu -Wl,-rpath,"$libdir" \
        -o "$APP/bin/cpp"
fi
run cpp

# And the C one, which is the half of this kit that is the ABI itself.
# It needs nothing out of this repository, and that is the point: it is
# the check that the SDK a user was handed is enough on its own.
mkdir -p "$APP/src-c"
lift c > "$APP/src-c/main.c"
test -s "$APP/src-c/main.c" || {
    echo "the README has no C program on it, which is a page to fix rather than an install to report" >&2
    exit 1
}
${CC:-cc} -std=c11 -O2 \
    -I"$SDK/include" \
    "$APP/src-c/main.c" \
    -L"$libdir" -lzu -Wl,-rpath,"$libdir" \
    -o "$APP/bin/c"
run c

echo "installed in $MODE mode and ran both programs on the page"

#!/bin/bash
# Builds Firebird on Linux from the Windows working tree and runs the
# LTRIM_ZERO collation tests.
#
#   run.sh            -> Release build (DefaultTarget=Release)
#   run.sh developer  -> --enable-developer, which makes DefaultTarget=Debug
#                        (builds/posix/Makefile.in:55-59) and compiles with
#                        -DDEV_BUILD (builds/posix/make.rules:63-67), turning
#                        the fb_assert at src/jrd/intl.cpp:398-399 back on.
#
# /src lives in a named volume so a retry does not rebuild from scratch.
# /repo is the Windows checkout, read only. Its files have CRLF endings, which
# breaks autogen.sh, so the tree is cloned from it (git rewrites the endings)
# and only the files that differ from the committed state are copied over.

set -e

MODE="${1:-release}"
CONFIGURE_FLAGS="--enable-binreloc --prefix=/opt/firebird"
[ "$MODE" = "developer" ] && CONFIGURE_FLAGS="--enable-developer $CONFIGURE_FLAGS"

echo "===== mode: $MODE"

if [ ! -d /src/.git ]; then
    echo "===== clone"
    # /src is a mount point, so empty it instead of removing it
    find /src -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    git clone --no-hardlinks --quiet /repo /src
else
    echo "===== reusing existing /src"
fi

cd /src
echo "branch: $(git rev-parse --abbrev-ref HEAD)  head: $(git rev-parse --short HEAD)"

echo "===== overlay working tree changes"
for f in src/intl/lc_ltrim_zero.cpp src/jrd/tests/LtrimZeroCollationTest.cpp test_ltrim_zero.sql; do
    tr -d '\r' < "/repo/$f" > "$f"
    echo "  $f"
done

echo "===== registration present in the committed tree"
grep -n "LCLTRIMZERO_init" src/intl/ld.cpp
grep -n "LTRIM_ZERO" builds/install/misc/fbintl.conf
grep -n "INTL_Objects" builds/posix/make.shared.variables

if [ ! -f gen/Makefile ] || [ ! -f /src/.configured_$MODE ]; then
    echo "===== autogen $CONFIGURE_FLAGS"
    rm -f /src/.configured_*
    ./autogen.sh $CONFIGURE_FLAGS > /tmp/configure.log 2>&1 || { tail -40 /tmp/configure.log; exit 1; }
    touch "/src/.configured_$MODE"
else
    echo "===== reusing existing configuration"
fi

echo "===== make"
make -j"$(nproc)" > /tmp/make.log 2>&1 || { tail -60 /tmp/make.log; exit 1; }
echo "  ok"

if [ "$MODE" = "developer" ]; then
    echo "  compile lines carrying -DDEV_BUILD: $(grep -c '\-DDEV_BUILD' /tmp/make.log || true)"
fi

echo "===== lc_ltrim_zero really compiled and linked?"
find . -name "lc_ltrim_zero*.o" | head
SO=$(find gen -name "libfbintl.so*" -type f | head -1)
echo "fbintl: $SO"
nm -a "$SO" 2>/dev/null | grep -i ltrimzero || echo "  (symbol not exported, expected: it is internal to the module)"

echo "===== make tests"
make tests -j"$(nproc)" > /tmp/tests.log 2>&1 || { tail -60 /tmp/tests.log; exit 1; }
echo "  ok"

set +e

echo "===== locate binaries"
# make.defaults:269 -> $(FB_BUILD)/tests/libEngine<ods>_test, not "engine_test"
TARGET_DIR=Release
[ "$MODE" = "developer" ] && TARGET_DIR=Debug
FBROOT="/src/gen/$TARGET_DIR/firebird"
ENGINE_TEST=$(ls "$FBROOT"/tests/libEngine*_test 2>/dev/null | head -1)
ISQL="$FBROOT/bin/isql"
echo "fb root:     $FBROOT"
echo "engine_test: ${ENGINE_TEST:-NOT FOUND}"
echo "isql:        $ISQL $([ -x "$ISQL" ] || echo NOT FOUND)"

if [ -n "$ENGINE_TEST" ]; then
    echo "===== LTRIM_ZERO suite"
    "$ENGINE_TEST" --run_test=EngineSuite/LtrimZeroSuite --log_level=message 2>&1 | tail -70
    echo "engine_test exit: ${PIPESTATUS[0]}"
fi

echo "===== whole run_tests, as CI does"
make run_tests 2>&1 | tail -8

if [ -x "$ISQL" ]; then
    echo "===== SQL suite"
    ls "$FBROOT/intl"
    cd /tmp
    rm -f test_ltrim.fdb
    FIREBIRD="$FBROOT" "$ISQL" -input /src/test_ltrim_zero.sql 2>&1 | tail -30
fi

echo "===== done ($MODE)"

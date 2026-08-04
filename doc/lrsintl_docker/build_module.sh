#!/bin/bash
# Builds the standalone lrsintl.so out of a checkout mounted at /repo,
# following the Linux recipe in doc/README.lrsintl_build.md.
#
# Run it inside the image built from the Dockerfile next to this file:
#
#   docker build -t idz-builder:22.04 doc/lrsintl_docker
#   docker run --rm \
#       -v "$PWD":/repo:ro -v idz_src:/src \
#       -v "$PWD/doc/lrsintl_docker":/scripts:ro -v "$PWD/out":/out \
#       idz-builder:22.04 bash /scripts/build_module.sh
#
# The tree is cloned into /src because a Windows checkout carries CRLF
# endings, which break autogen.sh. /src is a named volume so a second run
# reuses the clone and the configuration.
set -e

if [ ! -d /src/.git ]; then
    echo "===== clone"
    # /src is a mount point, so empty it instead of removing it
    find /src -mindepth 1 -maxdepth 1 -exec rm -rf {} +
    git clone --no-hardlinks --quiet /repo /src
else
    echo "===== reusing /src"
    cd /src && git fetch --quiet /repo HEAD && git reset --quiet --hard FETCH_HEAD
fi

cd /src
echo "head: $(git rev-parse --short HEAD)  $(git log -1 --format=%s)"

if [ ! -f src/include/gen/autoconfig.auto ]; then
    echo "===== configure (produces gen/autoconfig.auto)"
    ./autogen.sh --enable-binreloc --prefix=/opt/firebird > /tmp/configure.log 2>&1 \
        || { tail -40 /tmp/configure.log; exit 1; }
else
    echo "===== reusing existing gen/autoconfig.auto"
fi

# configure writes autoconfig.auto. autoconfig.h is a symlink to it that the
# build creates on the way in (builds/posix/Makefile.in:334), and firebird.h
# includes "gen/autoconfig.h" (src/include/firebird.h:38). Running the whole
# make just to get that symlink is not worth it.
ln -sf "$PWD/src/include/gen/autoconfig.auto" "$PWD/src/include/gen/autoconfig.h"
ls -l src/include/gen/autoconfig.h

echo "===== compile"
mkdir -p /out
g++ -O2 -std=c++17 -fPIC -fno-rtti -pipe -Wall -Wno-unused-parameter \
    -DLINUX -DAMD64 -DFB_SEND_FLAGS=MSG_NOSIGNAL \
    -I/src/src -I/src/src/include -I/src/src/include/gen \
    -shared -o /out/lrsintl.so /src/src/intl/lrsintl/ld_min.cpp
cp /src/src/intl/lrsintl/lrsintl.conf /out/
ls -l /out

echo "===== exported entry points"
nm -D --defined-only /out/lrsintl.so | grep -E 'LD_(version|lookup_texttype_with_status)' \
    || { echo "MISSING entry points"; exit 1; }

echo "===== shared library dependencies"
ldd /out/lrsintl.so

echo "===== required glibc symbol versions"
objdump -T /out/lrsintl.so | grep -o 'GLIBC_[0-9.]*' | sort -u

echo "===== builder glibc"
ldd --version | head -1

echo "===== done"

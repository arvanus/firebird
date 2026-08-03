# Building fbltrimzero, the standalone LTRIM_ZERO module

## What this is

`fbltrimzero` is a small INTL module that exposes only the `LTRIM_ZERO`
collations (`WIN1252_LTRIM_ZERO` and `ISO8859_1_LTRIM_ZERO`) as a shared
library separate from `fbintl`. It is built from a single source file,
`src/intl/ltrimzero/ld_min.cpp`, which `#include`s the collation driver,
`src/intl/lc_ltrim_zero.cpp`, straight from the tree so there is one source of
truth for the collation logic.

It exists because production installs run the stock engine, not a rebuilt
one: replacing `fbintl.dll`/`libfbintl.so` there would register the
`LTRIM_ZERO` collations a second time (once from the stock module, once from
the replacement) and the engine would refuse to load one of the two. Shipping
a second, narrowly scoped module avoids that clash entirely: it declares the
collations under its own `intl_module` name in its own `.conf` file, and the
charsets themselves (`WIN1252`, `ISO8859_1`) keep coming from `fbintl` as
always. See `src/intl/ltrimzero/fbltrimzero.conf` for the registration.

`src/intl/ltrimzero/` is deliberately outside every existing project/glob
that builds `src/intl`: `builds/win32/msvc15/intl.vcxproj` lists its sources
explicitly and does not mention this directory, and the POSIX object glob in
`builds/posix/make.shared.variables` only scans `src/intl/*.cpp`, not its
subdirectories. So `ld_min.cpp` is never compiled into `fbintl` itself, and
there is no duplicate `LD_version` symbol to resolve.

## Windows recipe

```
set VS170COMNTOOLS=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\
cd builds\win32
call setenvvar.bat
call make_ltrimzero.bat
```

`setenvvar.bat` sets `FB_ROOT_PATH`, which `make_ltrimzero.bat` requires.
The script compiles `src/intl/ltrimzero/ld_min.cpp` directly with `cl`, copies
`fbltrimzero.conf` alongside the binary, and prints the `dumpbin` checks
described below.

Output: `builds\win32\ltrimzero\fbltrimzero.dll` and
`builds\win32\ltrimzero\fbltrimzero.conf`.

## The `/MT` decision

`make_ltrimzero.bat`'s compile line mirrors the Release x64 settings of
`intl.vcxproj` and `FirebirdCommon.props`, including `/EHsc-`
(`FirebirdCommon.props:11`), with exactly one deliberate deviation: it links
the module against the static CRT (`/MT`) where those files use `/MD`. This
is deliberate: `/MT` means the module never depends on a Visual C++
redistributable being present on the target machine, which matters because
this module is meant to be dropped into a customer's existing Firebird
install rather than built and installed alongside it.

This is safe specifically because no CRT object crosses the module boundary.
The driver in `lc_ltrim_zero.cpp` allocates nothing, throws nothing and holds
no state; the two exported entry points only read and write buffers that the
engine itself owns. Mixing CRTs is a problem when one side frees memory the
other side allocated, or when C++ exceptions unwind across the boundary;
neither happens here.

`intl.vcxproj` also defines `INTL_EXPORTS` for `fbintl`, but that define is
unused: it appears in no `.rc` file and no source file in the tree, only in
`intl.vcxproj`'s own preprocessor definitions. `make_ltrimzero.bat` omits it
on purpose, and omitting it changes nothing observable.

## Verifying the build

After `make_ltrimzero.bat` finishes, it runs these checks itself and prints
the output. Confirm by eye:

```
dumpbin /nologo /exports fbltrimzero.dll
```

must list exactly two names, undecorated: `LD_version` and
`LD_lookup_texttype_with_status`. Undecorated names are what `ld_proto.h`'s
`extern "C"` wrapping and `FB_DLL_EXPORT` (`__declspec(dllexport)` on
Windows) produce; no `.def` file is needed.

```
dumpbin /nologo /dependents fbltrimzero.dll
```

must list only `KERNEL32.dll`. If `VCRUNTIME140.dll` or `MSVCP140.dll` show
up instead, `/MT` did not take effect and the module would carry a
redistributable dependency; fix the compile line before shipping it.

Note that these two checks prove the module exports the right symbols with
the right dependency footprint. They do not by themselves prove the engine
resolves those symbols at load time; that has to be confirmed separately, by
starting a server with the module installed and running `CREATE COLLATION
... FROM EXTERNAL`.

## Linux recipe

There is no CMake or Autotools target for this module; it is built with a
plain Makefile that lives with the module's own repository (not inside this
tree), because it needs a Firebird source tree that has already been
configured: `firebird.h` includes `gen/autoconfig.h`
(`src/include/firebird.h:38`), and on POSIX that file is produced by the
build, unlike the Windows build, where `autoconfig_msvc.h` is already checked
in.

Careful with what `./configure` actually leaves behind. It writes
`src/include/gen/autoconfig.auto`, not `autoconfig.h`; `autoconfig.h` is a
symlink to it that the makefile creates on the way in
(`builds/posix/Makefile.in:334`). A tree that was only configured, never
built, therefore fails the Makefile's own precondition check. Either run the
build once, or create the link:

```bash
ln -sf "$PWD/src/include/gen/autoconfig.auto" "$PWD/src/include/gen/autoconfig.h"
```

The Makefile:

```make
FB_SRC ?= ../firebird

TARGET  := fbltrimzero.so

# Same platform defines the POSIX build uses for src/intl.
DEFS    := -DLINUX -DAMD64 -DFB_SEND_FLAGS=MSG_NOSIGNAL
INCLUDES := -I$(FB_SRC)/src -I$(FB_SRC)/src/include -I$(FB_SRC)/src/include/gen
CXXFLAGS ?= -O2 -std=c++17 -fPIC -fno-rtti -pipe -Wall -Wno-unused-parameter
LDFLAGS  ?= -shared

.PHONY: all clean check

all: $(TARGET)

$(TARGET): ld_min.cpp $(FB_SRC)/src/intl/lc_ltrim_zero.cpp
	@test -f "$(FB_SRC)/src/include/gen/autoconfig.h" || { \
		echo "ERROR: $(FB_SRC) is not configured. Run ./autogen.sh there first."; \
		exit 1; }
	$(CXX) $(CXXFLAGS) $(DEFS) $(INCLUDES) $(LDFLAGS) -o $@ ld_min.cpp

check: $(TARGET)
	@echo "--- exported entry points"
	@nm -D --defined-only $(TARGET) | grep -E 'LD_(version|lookup_texttype_with_status)' \
		|| { echo "MISSING entry points"; exit 1; }
	@echo "--- shared library dependencies"
	@ldd $(TARGET)
	@echo "--- required glibc symbol versions"
	@objdump -T $(TARGET) | grep -o 'GLIBC_[0-9.]*' | sort -u

clean:
	rm -f $(TARGET)
```

Usage:

```bash
make FB_SRC=/path/to/configured/firebird
make FB_SRC=/path/to/configured/firebird check
```

`check` runs the same kind of verification as the Windows `dumpbin` calls:
`nm -D --defined-only` confirms both entry points are exported, `ldd` lists
the shared library dependencies (expected to be libc and the dynamic linker
only, mirroring the Windows `KERNEL32.dll`-only result), and `objdump -T`
lists the glibc symbol versions the binary requires, so a build on a newer
glibc can be checked against an older target before shipping it. The actual
size and the glibc baseline depend on the toolchain used to build; treat any
number quoted for a specific build as a property of that build, not of the
recipe.

### Building it from a Windows checkout, in a container

`doc/ltrim_zero_docker/build_module.sh` does the whole Linux recipe without a
Linux machine: it clones the checkout mounted at `/repo` (a Windows tree has
CRLF endings, which break `autogen.sh`), configures it, creates the
`autoconfig.h` link, compiles, and runs the three checks.

```bash
docker build -t ltz-builder:22.04 doc/ltrim_zero_docker
docker run --rm \
    -v "$PWD":/repo:ro -v ltz_src:/src \
    -v "$PWD/doc/ltrim_zero_docker":/scripts:ro -v "$PWD/out":/out \
    ltz-builder:22.04 bash /scripts/build_module.sh
```

`/src` is a named volume, so a second run reuses the clone and the
configuration instead of paying for both again.

Measured for the build made on 2026-08-03 from that image (Ubuntu 22.04,
glibc 2.35): 16200 bytes, both entry points exported, `libc.so.6` plus the
dynamic linker as the only dependencies, and `GLIBC_2.2.5` as the single
symbol version required. That last number is what makes one binary serve
every current target: verified loading on Rocky Linux 9.8 (glibc 2.34) with
both entry points resolving, and running the full SQL suite (55/55) inside
`firebirdsql/firebird:5.0.3-noble` (glibc 2.39).

### Baking it into a Firebird docker image

`doc/ltrim_zero_docker/image/Dockerfile` derives from the official image and
only drops the two files into `/opt/firebird/intl`:

```bash
cp out/fbltrimzero.so out/fbltrimzero.conf doc/ltrim_zero_docker/image/
docker build -t srs/firebird:5.0.3-noble-ltrimzero doc/ltrim_zero_docker/image
```

To move it to a server without a registry:

```bash
docker save srs/firebird:5.0.3-noble-ltrimzero | gzip > fb-ltz.tar.gz
# on the server
gunzip -c fb-ltz.tar.gz | docker load
```

## Installing

Copy the built module and its `.conf` file into the `intl` directory of the
target installation, next to `fbintl.dll`/`libfbintl.so` and `fbintl.conf`:

- Windows: `fbltrimzero.dll` and `fbltrimzero.conf`
- Linux: `fbltrimzero.so` and `fbltrimzero.conf`

Do **not** edit `fbintl.conf`. The engine scans every `*.conf` file in the
`intl` directory on its own (`Jrd::IntlManager::initialize`, which does
`ScanDir(intlPath, "*.conf")`), so `fbltrimzero.conf` is picked up as long as
it is present; if the target's `fbintl.conf` does not already `#include` it,
add an `#include` line for it there instead of inlining the module's
declarations into `fbintl.conf`.

After copying, restart the server so it re-scans the `intl` directory, then
in each database:

```sql
CREATE COLLATION WIN1252_LTRIM_ZERO FOR WIN1252
    FROM EXTERNAL ('WIN1252_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE;

CREATE COLLATION ISO8859_1_LTRIM_ZERO FOR ISO8859_1
    FROM EXTERNAL ('ISO8859_1_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE;
```

Only `fbltrimzero.dll`/`fbltrimzero.so` is ever replaced in a target
installation's `intl` directory. `fbintl.dll`/`libfbintl.so` is never
touched: it keeps owning the `WIN1252` and `ISO8859_1` charsets themselves,
and swapping it would risk registering the collations twice.

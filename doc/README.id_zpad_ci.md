# ID_ZPAD_CI collation

Single byte collation for `WIN1252` and `ISO8859_1` that ignores leading zeros
and spaces and compares case-insensitively over the ASCII range.

```
"00000A" = "0A" = "A" = "    A" = "a" = "000a  "
```

Implementation: `src/intl/lc_id_zpad_ci.cpp`
Driver registration: `src/intl/ld.cpp`
Runtime registration: `builds/install/misc/fbintl.conf`
Test suite: `test_id_zpad_ci.sql` (run it with `run_test.bat`)

---

## 1. DDL

```sql
CREATE COLLATION WIN1252_ID_ZPAD_CI FOR WIN1252
    FROM EXTERNAL ('WIN1252_ID_ZPAD_CI') CASE INSENSITIVE PAD SPACE;

CREATE COLLATION ISO8859_1_ID_ZPAD_CI FOR ISO8859_1
    FROM EXTERNAL ('ISO8859_1_ID_ZPAD_CI') CASE INSENSITIVE PAD SPACE;
```

**Both clauses are mandatory in practice:**

- `PAD SPACE` is what makes a `CHAR` column work. Without it, `'A'` stored in
  `CHAR(5)` is physically `"A    "` and never matches the literal `'A'`.
- `CASE INSENSITIVE` is what keeps `SIMILAR TO` in line with `=`, `LIKE` and
  `CONTAINING`. `SIMILAR TO` does not go through `texttype_fn_canonical`: it
  compiles to RE2 and reads case insensitivity straight from the collation
  attribute (`src/jrd/Collation.cpp:132`). Without the attribute, `SIMILAR TO`
  stays case-sensitive while everything else does not.

`ACCENT INSENSITIVE` is rejected on purpose: comparison is ASCII-only, and
accepting the attribute would make `SIMILAR TO` fold accents while `=` does
not.

Typical use:

```sql
CREATE DOMAIN CODIGO_ZPAD AS VARCHAR(50)
    CHARACTER SET WIN1252 COLLATE WIN1252_ID_ZPAD_CI;
```

---

## 2. Exact semantics

Call `N(x)` the normalized form used by both the comparison and the index key:

1. drop trailing spaces (only under `PAD SPACE`);
2. drop leading `'0'` and `' '`, in any interleaving, up to the first character
   that is neither;
3. fold `a-z` to `A-Z`.

Consequences:

| input | `N` | note |
|---|---|---|
| `'00000A'`, `'0A'`, `'A'`, `'    A'`, `'a'`, `'000a  '` | `A` | one equivalence class |
| `'0 A 0'` | `A 0` | the cut stops at the first real character |
| `'A00'`, `'00A00'` | `A00` | a trailing zero is significant |
| `'000'`, `'   '`, `' 0 '`, `''` | empty | anything made only of zeros and spaces becomes the empty string |
| `NULL` | - | `NULL` stays `NULL`, it does not join the empty class |

The relation is a true equivalence (`N(a) == N(b)` for a pure function `N`), so
`=`, `DISTINCT`, `GROUP BY`, `UNIQUE` and the index order always agree - except
at the `MAX_KEY` boundary documented in section 3, where `DISTINCT` decides from
the truncated key and can diverge from `=`. That is why the collation does
**not** need `TEXTTYPE_SEPARATE_UNIQUE`.

The index key is no longer the normalized string on its own. It is now

```
[2 bytes: length of N(x), big endian][bytes of N(x), upper cased]
```

The length prefix is what makes the ordering go by length first (see section 3).
Two entries stay in the same equivalence class exactly when `N(a) == N(b)`,
because equal normalized strings have equal length, so the key remains a pure
function of `N(x)`, and `=`/`DISTINCT`/`GROUP BY`/`UNIQUE` keep agreeing with
each other and with the index order, with the same `MAX_KEY` exception from
section 3.

> When editing the driver, keep `texttype_fn_compare` and
> `texttype_fn_string_to_key` on the same `normalize_bounds`. If the two ever
> diverge, `UNIQUE` and `DISTINCT` silently stop agreeing with `=`.

---

## 3. Known limits

**Ordering is by the length of the normalized form first, then byte by byte
over the normalized form, upper cased.** `'9' < '10'`, which matches what
anyone adopting a "strip the leading zero" collation expects. Within the same
length the order falls back to plain byte order, so a digit comes before a
letter (`'9' = 0x39 < 'A' = 0x41`). The length that counts is that of `N(x)`,
the normalized form, not that of the stored value, which is what puts `'9'` and
`'0000009'` in the same position:

| stored value | normalized | length | position |
|---|---|---|---|
| `'000'` | (empty) | 0 | 1st |
| `'9'` | `9` | 1 | 2nd, tied |
| `'0000009'` | `9` | 1 | 2nd, tied |
| `'A'` | `A` | 1 | 4th |
| `'10'` | `10` | 2 | 5th |
| `'0001A34'` | `1A34` | 4 | 6th |
| `'12345678901'` | `12345678901` | 11 | 7th |
| `'12345678000199'` | `12345678000199` | 14 | 8th |

Equality did not change: `'000123' = '123'` is still true, and the equivalence
class is still the normalized form. Values differing only in leading zeros
still compare equal, and still collide in a `UNIQUE` index, exactly as before.

**`BETWEEN` changed meaning beyond the CNPJ case that motivated ordering by
length.** `BETWEEN '9' AND '11'` now includes every value of normalized length
1 greater than `'9'`, letters included, before reaching `'10'`: `'A'`, `'B'`,
`'Z'` satisfy the range, because they tie with `'9'` on length and win on the
byte, while staying shorter than the upper bound `'11'`, of length 2. Anyone
filtering a mixed-width column with `BETWEEN` has to account for this, not just
for the CNPJ root search case.

**The collation knows nothing about a sign, and none of this holds for a
negative value.** The cut in rule 2 stops at the first character that is
neither `'0'` nor `' '`, and `-` (0x2D) is neither. So the sign shields
everything that follows it:

| stored value | `N` | length |
|---|---|---|
| `'-3'` | `-3` | 2 |
| `'00-03'` | `-03` | 3, the zeros **before** the sign go |
| `'-0003'` | `-0003` | 5, the zeros **after** the sign stay |

Measured: `'-0003' <> '-3'` and `'00-03' <> '-3'`, but `'00-03' = '-03'`. The
property the collation is named after does not reach negative values, because
there the zeros are no longer the first characters of the string.

Ordering is not numeric for negatives either, on two axes at once. Measured
with `ORDER BY ... COLLATE ISO8859_1_ID_ZPAD_CI`:

```
5 < 9 < -1 < -3 < 00-03 < -20 < 300 < -100 < -0003
```

Every negative lands after every shorter positive, because the sign counts as a
character in the length; and among negatives the order comes out by increasing
magnitude, the inverse of numeric order. Ordering by length made this case
worse: the old byte-by-byte comparison put `'-9' < '9'` by accident, since
`-` (0x2D) < `9` (0x39). The broken equality of `'-0003'` against `'-3'` already
existed before and did not change.

Usage conclusion: the collation is for a textual identifier (CNPJ, CPF,
registration code), not for a signed number. A column that has to hold a
negative with numeric semantics wants a real numeric column, or an expression
index over a normalized form, not this collation.

On `SCHERER_001` this is academic. Scanned on 2026-08-04: no row carries a sign
in `ENTIDADE` (544149 rows), `ENTIDADE_COBRANCA` or `PRODUTO.FORNECEDOR`; one
row in `ENTIDADE_OBSERVACAO`, holding `-922337203685478`, inherited garbage from
when the domain was numeric. That row is found both by the index and by the
scan, so it is ugly, not inconsistent. The other 364 columns of the domain were
not scanned.

**Above `MAX_KEY` (8192 bytes), `DISTINCT` (and an `ORDER BY` tie) can stop
telling apart two values that differ only in the last 2 bytes; `GROUP BY` does
not.** `MAX_KEY` is a compile-time constant (`constants.h:195`), so this limit
does not depend on page size: verified identical on 8192 and 32768 byte pages.
The engine has always cut the sort key of a value wider than `MAX_KEY` at the
raw field length, silently truncating the tail (`intl.cpp:1002-1019`).

Truncation depends on the **declared column width**, not only on the normalized
length of a value: `INTL_key_length` sizes the sort key space from the raw field
length, the same for every row of the column. A normalized length of 8191 is
necessary for truncation to be possible (below that no column width triggers
it), but not sufficient: in a `VARCHAR(12000)` column, `dstLen` is 12000 for
every row, so truncation only starts at normalized length 11999 (verified:
11998 does not collide under `DISTINCT`, 11999 does), not at 8191.

`DISTINCT` decides equality from the truncated key, but not through
`SortedStream::compareKeys` - that function is only called by merge join
(`MergeJoin.cpp:273`), unrelated to `DISTINCT`. The real path: when
`FLAG_PROJECT` is on, `SortedStream::init` (`SortedStream.cpp:190`) passes
`RecordSource::rejectDuplicate` (`RecordSource.h:104`, unconditionally returns
`true`) as the sort's duplicate callback, fired by `DO_32_COMPARE` over the raw
key in `sort.cpp:1301-1312` whenever two adjacent keys compare equal byte for
byte. There is no value revalidation anywhere on that path - not even the
`FLAG_KEY_VARY` return from CORE-4909 that `SortedStream::compareKeys` has
(`SortedStream.cpp:286-317`). That is why two different values colliding on the
truncated key merge under `DISTINCT`.

`GROUP BY` does not: it revalidates with a real value compare when deciding
whether a new group started (`AggregatedStream.cpp:308-345`, `lookForChange`,
`MOV_compare` on line 345), so it stays correct regardless of key truncation,
always.

A real index never reaches that regime, and the margin is generous: `CREATE
INDEX` computes `key_length = ROUNDUP(INTL_key_length(len) + 1, 8)` (the `+1` is
the NULL indicator byte, `idx.cpp:876-877`) and refuses when that is greater
than or equal to `page_size / 4` (`idx.cpp:879`, `Database.h:654`). At the
largest page size (32768, ceiling 8192), verified: `VARCHAR(8181)` is created,
`VARCHAR(8182)` fails with "key size exceeds implementation restriction", and
`VARCHAR(8190)` fails even earlier, in a coarser `MAX_KEY` check in the DSQL
layer (`DdlNodes.epp`) with "key size too big for index". The widest column
still indexable is `VARCHAR(8181)`, a full 10 bytes below where sort key
truncation would begin. Only a sort key (`ORDER BY`, `GROUP BY`, `DISTINCT`),
which has no `page_size / 4` ceiling, reaches that regime. The narrowest column
able to get there is far wider than the 20 bytes of the `TDR_CNPJ` domain; the
customer's database is not affected.

**Case-insensitive over ASCII only.** Registered for WIN1252/ISO8859_1, but
`'é' <> 'É'` under comparison. `UPPER()` and `LOWER()` stay correct with accents,
because the driver deliberately does not install `texttype_fn_str_to_upper` /
`_str_to_lower` and lets the engine's ICU handle it.

**`LIKE` / `CONTAINING` / `SIMILAR TO` are not zero-insensitive.** Those
operators go through `texttype_fn_canonical`, which is a **per-character**
mapping and by construction cannot remove leading zeros. The canonical form here
is only the uppercasing. So `'00000A' LIKE 'A'` is false, even though
`'00000A' = 'A'` is true. If you need a zero-insensitive prefix search, use an
expression index or a persisted normalized column.

**`STARTING WITH` and `LIKE 'x%'` over an indexed column now scan the whole
index, not just the matching range.** Since the key carries a length prefix, the
key of a prefix is no longer a byte prefix of the key of the whole value, so a
partial key can no longer describe "starts with" at all. `string_to_key` returns
an empty key for any prefix in that case, which the engine treats as "no filter
coming from the index": it walks every entry and re-applies the predicate after
the fetch. This used to happen only for a prefix that normalizes entirely to
empty (such as `'00'`); it now happens for every `STARTING WITH` and
`LIKE 'x%'`, whatever the prefix. The result stays correct: the natural plan and
the index plan return the same set, no row is lost, and this is covered by
asserts 8.6, 8.6b, 8.6c and 8.7. The plan does get worse, though: the optimizer
still estimates the range as selective and does not know the scan itself is now
complete.

**`UNIQUE` follows the collation.** Inserting `'0A'` after `'A'` is a key
violation. That is the requested semantics, but it has to be clear to whoever
models the schema.

---

## 4. Deploying on an existing database - read before swapping the library

The normalized form defines the bytes of the index key. **Any change to the
normalization invalidates every index already built over an `ID_ZPAD_CI`
column**, and the engine does not validate an index against a collation
version: the symptom is a row that is not found, with no error at all.

When replacing `lrsintl.dll` / `lrsintl.so` on a database that already has such
indexes, the path is a `gbak` backup/restore: the restore rebuilds every key by
calling the new driver, with no exception by index type.

Rebuilding index by index does not solve it. `ALTER INDEX ... INACTIVE` is
refused for a constraint index (`Cannot deactivate index used by a
PRIMARY/UNIQUE constraint`, a system trigger registered in
`src/jrd/ini.epp:188-190`), and it is precisely the implicit index of
`PRIMARY KEY` and `UNIQUE` that cannot stay in the old format: an old key with a
new driver returns no row and raises no error.

See `doc/README.id_zpad_ci_rollout.md` for anyone who already has indexes built
in the old key format (without the length prefix) and needs the inventory and
the runbook for the swap.

---

## 5. Build

No source list has to be maintained by hand, except under MSVC:

- POSIX: `builds/posix/make.shared.variables:4-7,137` globs `src/intl/*.cpp`.
- CMake: `src/CMakeLists.txt:518` globs, **without `CONFIGURE_DEPENDS`**. An
  existing CMake build tree keeps producing an `fbintl` without the new object
  until someone runs `cmake` again.
- MSVC: `builds/win32/msvc15/intl.vcxproj` and `.filters` list the file
  explicitly.
- `builds/posix/fbintl.vers` needs no change: `LCIDZPADCI_init` is only
  referenced from inside `fbintl` itself, exporting it would be wrong.

---

## 6. Tests

There are three, with non-overlapping scopes.

### 6.1 Semantics - `test_id_zpad_ci.sql`

```bat
msbuild builds\win32\msvc15\intl.vcxproj /p:Configuration=Release /p:Platform=x64
run_test.bat
```

Self-verifying: each check writes a row into `TST` and the footer prints
`SUMMARY` and `VERDICT`. It passes when `*** SUITE PASSED ***` comes out with
`FAILED = 0`. Checks marked `I` (info) document the limits from section 3 and
never fail the run.

Covers: the equivalence class, `PAD SPACE` on `CHAR`/`VARCHAR`, the `NO PAD`
variant, empty class versus `NULL`, agreement between the natural plan and the
index plan (`=`, `STARTING WITH`, `ORDER BY`, join), `UNIQUE`, 32600 byte
values, normalized keys above `MAX_KEY` compared against the default collation
as a control, `UPPER`/`LOWER` with accents, `LIKE`/`CONTAINING`/`SIMILAR TO`,
and the registration under `ISO8859_1`.

### 6.2 Key and ordering - `src/jrd/tests/IdZpadKeyTest.cpp`

Calls the driver directly, in process, with no server and no database. It owns
the one invariant everything else rests on: the sign of `compare()` must equal
the sign of a plain byte comparison of the two sort keys. If those ever
disagree, an index stops describing the order the engine believes it describes,
and `UNIQUE`, `DISTINCT` and `BETWEEN` all start lying.

```bat
temp\x64\Release\firebird\engine_test.exe --run_test=IntlSuite/IdZpadKeySuite
```

A private copy of the driver is compiled into the test, with the entry point
renamed, because the functions behind the texttype vtable are `static` and
cannot be reached any other way. The key buffer is poisoned with `0xCC` before
each call, so a key built out of uninitialized memory shows up instead of
silently passing.

### 6.3 Integration - `src/jrd/tests/IdZpadCollationTest.cpp`

Part of `engine_test` (Boost.Test). On POSIX it is picked up by the glob
(`make.shared.variables:93`); under MSVC it is listed in `engine_test.vcxproj`.

```bat
msbuild builds\win32\msvc15\engine_test.vcxproj /p:Configuration=Release /p:Platform=x64
temp\x64\Release\firebird\engine_test.exe --run_test=EngineSuite/IdZpadSuite
```

```bash
make run_tests                                             # everything
./gen/.../engine_test --run_test=EngineSuite/IdZpadSuite   # this suite only
```

Covers what an isql script cannot reach:

| Case | What it exercises |
|---|---|
| `ConcurrentVolumeAndValidation` | 8 simultaneous attachments inserting 50000 rows into an indexed table, then an online validation of the database. Page splits, multi-level b-tree, prefix compression between nodes |
| `DescendingIndexAndEmptyKey` | `DESCENDING` index with an empty key (`btr.cpp:2941-2944`), descending navigation against an inverted ascending sort |
| `CompoundIndex` | `key_empty`/`key_nulls` per segment in a two-column index |
| `PrimaryAndForeignKey` | PK collision between `'A'` and `'000a  '`, FK from `'0A'` to `'A'`, parent delete blocked by a child |
| `BackupRestoreRoundTrip` | `gbak` backup/restore through the Services API. The restore rebuilds every index by calling `string_to_key` again - this is the deployment path from section 4 |
| `BlobAndTransliteration` | `CONTAINING` on a `BLOB SUB_TYPE TEXT` with the collation, and comparison against `_UTF8` / `_ISO8859_1` literals |

All of them compare **row identity** (an ordered list of IDs) between the
natural plan and the index plan, not just counts.

The first case, `CollationIsInstalled`, runs before everything else and fails
loudly when the runtime directory is incomplete. It creates a **control**
collation (`PXW_SPAN`, which lives in the same block of `fbintl.conf`) before
trying `ID_ZPAD_CI`: if the control fails too, the message says that this tree
cannot register anything coming from `fbintl.conf` and that the suite is
inconclusive - instead of looking like a bug in the collation.

### 6.4 Linux and DEV_BUILD, through Docker

`doc/lrsintl_docker/` carries the harness used to validate on Linux. It clones
the repository from inside the container - a Windows tree has CRLF endings and
that breaks `autogen.sh` - and then overlays only the files that differ from the
commit.

```bat
docker build -t fb-idz-build doc\lrsintl_docker
docker volume create fb-idz-src

REM Release
docker run --rm -v "%CD%:/repo:ro" -v "%CD%\doc\lrsintl_docker:/work:ro" ^
    -v fb-idz-src:/src fb-idz-build bash /work/run.sh release

REM DEV_BUILD: --enable-developer makes DefaultTarget=Debug
REM (builds/posix/Makefile.in:55-59) and compiles with -DDEV_BUILD
REM (builds/posix/make.rules:63-67)
docker run --rm -v "%CD%:/repo:ro" -v "%CD%\doc\lrsintl_docker:/work:ro" ^
    -v fb-idz-src:/src fb-idz-build bash /work/run.sh developer
```

The `fb-idz-src` volume keeps the built tree between runs, so a second round
does not recompile everything.

The `developer` pass is the strongest verification available: it re-enables the
`fb_assert` in `src/jrd/intl.cpp:398-399`, which requires
`texttype_canonical_width` and `texttype_fn_canonical` to be defined together.
Had the collation got that pair wrong, the run would abort instead of passing.

Result of the two passes (Ubuntu 22.04, clang, x64):

| | Release | `--enable-developer` |
|---|---|---|
| `make` | ok | ok, 376 lines with `-DDEV_BUILD` |
| `lc_id_zpad_ci.o` produced | yes, by the glob, with no build file change | yes |
| `LCIDZPADCI_init` in `libfbintl.so` | local symbol, not exported | same |
| C++ suite | exit 0, no errors | exit 0, no errors |
| full `make run_tests` | passed | passed |
| online database validation | 0 errors | 0 errors |
| SQL suite | 55/55 | 55/55 |
| `fb_assert` from `intl.cpp:398-399` | inactive (Release) | **did not fire** |

---

## 7. Implementation notes

The driver is *stateless*: no mutable state, `texttype_impl` stays `NULL` and
there is no `texttype_fn_destroy`. It is therefore reentrant and safe under
heavy concurrency.

`texttype_fn_compare` and `texttype_fn_string_to_key` are single pass, with no
temporary buffer and no allocation. That is a requirement, not an optimization:

- the INTL callbacks run on every comparison and every index key, and the engine
  invokes them **without** any exception barrier (`Jrd::TextType`), so they must
  not throw;
- allocating from `getDefaultMemoryPool()` would take the process-wide pool mutex
  on every comparison, serializing the SuperServer threads;
- `string_to_key` receives `dstLen = 32767` on the index path
  (`src/jrd/btr.cpp:2880`) regardless of the value's length, so filling the rest
  of the buffer would cost a ~32 KB `memset` per key. The engine uses only the
  returned length.

`texttype_canonical_width` and `texttype_fn_canonical` have to be defined
together: `src/jrd/intl.cpp:398-399` has an `fb_assert` about it, and DEV_BUILD
is the default developer build on Linux. Under Release the assert disappears,
but leaving `fn_canonical` null with a non-zero width turns off
`TEXTTYPE_DIRECT_MATCH` and drops pattern matching into the identity `memcpy`,
leaving `LIKE` case-sensitive while `=` is case-insensitive.

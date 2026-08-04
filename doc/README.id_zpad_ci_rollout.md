# Rolling out the numeric ordering of ID_ZPAD_CI

The collation's ordering changed: the key now carries the normalized length
before the bytes, so `9` comes before `10` instead of after. The key format and
the motivation are in [README.id_zpad_ci.md](README.id_zpad_ci.md); the build
recipe for the module is in [README.lrsintl_build.md](README.lrsintl_build.md).

This document is the swap window: what breaks, why the path is backup and
restore, the exact sequence, and how to check afterwards.

Numbers quoted here were measured on 2026-08-03 against
`D:\u\banco\scherer\SCHERER_001.FDB` with the installed Firebird 5.0.

> **Rename note.** The rehearsal recorded below ran while the collation was
> still named `WIN1252_LTRIM_ZERO` / `ISO8859_1_LTRIM_ZERO` and the module was
> `fbltrimzero.dll`. Nothing measured changes because of the rename: the driver,
> the key format and the timings are the same. The runbook steps use the current
> names, but note that a database which has **not** been migrated yet still
> carries the old external name in `RDB$COLLATIONS.RDB$BASE_COLLATION_NAME`, and
> the inventory script matches both.

## 0. Inventory first

Run [id_zpad_ci_rollout_inventory.sql](id_zpad_ci_rollout_inventory.sql) on
**every database of the server**, not only the one known to be affected. The
module is shared by the whole installation.

```
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" -u SYSDBA -p masterkey ^
  <database path> ^
  -i doc\id_zpad_ci_rollout_inventory.sql ^
  -o inventory_<database>.txt
```

The inventory resolves everything through
`RDB$COLLATIONS.RDB$BASE_COLLATION_NAME`, which holds the name given to
`FROM EXTERNAL` (`src/dsql/DdlNodes.epp:3977`), never through the local name:
`CREATE COLLATION ... FROM EXTERNAL` lets the DBA call the collation whatever
they want, and this repository's own suite uses the name `IDZ`. Looking for
`ISO8859_1_ID_ZPAD_CI` in `RDB$COLLATION_NAME` would silently miss that
database.

Result on `SCHERER_001`:

| item | value |
|---|---|
| collations from the module | 1 (`ISO8859_1_LTRIM_ZERO`, id 126, charset 21) |
| affected columns | 368, all from the `TDR_CNPJ` domain |
| index segments | 421 (216 PRIMARY KEY, 2 UNIQUE, 203 without a constraint) |
| segments in a unique index | 220, of which 2 unique indexes without a constraint |
| descending segments | 2 |
| segments in an inactive index | 0 |
| expression indexes in the database | 38 |
| key headroom | `KEY_LIMIT` 4096 (16384 byte page) against an estimated peak of 778 bytes |

Of the 16 databases in `D:\u\banco\scherer`, only `SCHERER_001` has the
collation. All the others returned zero on query 1.

Query 5 is a coarse upper bound, not the engine's own arithmetic: each affected
segment grows by 2 bytes, a descending index adds 1, and a compound index
amplifies by about 25% of the stuff bytes (`src/jrd/btr.cpp:1727`). The limit is
`page_size / 4` (`src/jrd/Database.h:652-655`). In this database the headroom is
more than fivefold, so no index enters the truncation regime.

Query 4 exists because an expression index stores no row in
`RDB$INDEX_SEGMENTS` (confirmed: 38 expression indexes, 0 segments), so query 3
sees none of them. The engine builds the key of those indexes from the result of
the expression, through the single-segment path (`src/jrd/btr.cpp:2059`), so
what decides is the collation of the result. The source of each one has to be
read by hand and cross-checked against the list from query 2.

## 1. What breaks

Every index over a column with the collation stores keys in the old format.
After the library is swapped, those indexes describe an order the driver no
longer speaks, and the engine has no way to notice: an index key is opaque
bytes, with no version stored.

Measured on `SCHERER_001` on 2026-08-03, with the new library installed and the
indexes still as they were:

| query | through the index | through a scan (`CNPJ \|\| ''`) |
|---|---|---|
| `CNPJ = '06056181000154'` | 0 rows | 1 row |
| `CNPJ BETWEEN '10000000000000' AND '99999999999999'` | 0 rows | 139433 rows |
| `SELECT CNPJ FROM ENTIDADE ORDER BY CNPJ` | 0 of 544149 | 544149 |

The last row uses no `PLAN` at all: the optimizer picks index navigation on its
own and the query returns zero rows out of 544149. Forcing
`PLAN (ENTIDADE ORDER PK_ENTIDADE)` gives the same zero;
`PLAN (ENTIDADE NATURAL)` returns all 544149.

None of these queries raised an error. The database answers fast and answers
wrong. That is the state the installation sits in between the library swap and
the end of the index rebuild.

## 2. Why backup and restore, and not `ALTER INDEX`

Rebuilding index by index would require deactivating and reactivating each one,
and `ALTER INDEX ... INACTIVE` is refused for a constraint index:

```
Statement failed, SQLSTATE = 27000
unsuccessful metadata update
-ALTER INDEX RDB$PRIMARY1 failed
-action cancelled by trigger (3) to preserve data integrity
-Cannot deactivate index used by a PRIMARY/UNIQUE constraint
```

The system trigger `RDB$TRIGGER_20` is registered three times, as
`integ_index_mod`, `integ_index_deactivate` and `integ_deactivate_primary`
(`src/jrd/ini.epp:188-190`); the message texts live in the commented reference
block of `trigger20`, in `src/jrd/trig.h:1376-1378`. A plain index, without a
constraint, deactivates normally.

Of the 421 segments, 218 are under a constraint (216 from PRIMARY KEY and 2 from
UNIQUE). A selective rebuild would leave exactly those in the old format with
the new library, which is the wrong-result state from section 1, not the slow
state. That leaves 203 segments without a constraint, which could be handled
through `ALTER INDEX`, but doing half the work does not help.

The restore rebuilds every index by calling the driver again, which is already
the new one. It is a single operation, with no exception by index type.

## 3. No writes between the library swap and the end of the rebuild

The window is **full unavailability**, not read-only.

**Uniqueness.** `insert_key` hands the new key to `BTR_insert`, which decides
who is a duplicate candidate by comparing against the keys already written on
the page, still in the old format; `check_duplicates` only sees what BTR flagged
(`src/jrd/idx.cpp:2112-2119`). With 218 constraint segments plus 2 unique
indexes without a constraint, an `INSERT` inside that window can write a real
duplicate into a PRIMARY KEY with no error at all.

**Referential integrity.** `check_foreign_key` builds the new key and looks it
up in the partner's index through `BTR_evaluate`, also in the old format
(`src/jrd/idx.cpp:1942-1971`). On `SCHERER_001` this does not apply: the domain
takes part in no FK. It does apply to any other database the inventory points
at, and that is why section 9 is not a formality.

In both cases the violation goes in silently and only shows up later, when the
rebuilt index starts seeing the conflict.

## 4. Window sequence

1. **Stop the application** and confirm no connection is left:

   ```sql
   SELECT COUNT(*) FROM MON$ATTACHMENTS;
   ```

   Remember that `gbak -PAR` opens one connection per worker, so this counter
   inflates during the backup.

2. **Back up the current state.** It is also the rollback.

   ```
   "C:\Program Files\Firebird\Firebird_5_0\gbak.exe" -b -PAR 5 -v ^
     -y D:\u\banco\scherer\bkp_pre_numeric.log ^
     D:\u\banco\scherer\SCHERER_001.FDB ^
     D:\u\banco\scherer\scherer_001_pre_numeric.fbk
   ```

   The installed gbak is fine. The gbak built from this repository is only
   needed for `-FIX_DOMAINS`, which was already used in the domain conversion
   and does not come into this window.

3. **Swap the library**, with the service stopped:

   ```powershell
   Stop-Service FirebirdServerDefaultInstance
   $intl = 'C:\Program Files\Firebird\Firebird_5_0\intl'
   Copy-Item "$intl\lrsintl.dll" "$intl\lrsintl.dll.bak" -Force
   Copy-Item 'D:\u\banco\scherer\lrsintl_numeric-order.dll' "$intl\lrsintl.dll" -Force
   Start-Service FirebirdServerDefaultInstance
   ```

   The validated binary is the one from Task 5 of the plan, produced by
   `builds\win32\make_lrsintl.bat` and checked against the SQL suite running on
   a stock engine. `lrsintl.conf` does not change and does not need to be copied
   again.

4. **Restore into a new path:**

   ```
   "C:\Program Files\Firebird\Firebird_5_0\gbak.exe" -c -PAR 5 -v ^
     -y D:\u\banco\scherer\restore_numeric.log ^
     D:\u\banco\scherer\scherer_001_pre_numeric.fbk ^
     D:\u\banco\scherer\SCHERER_001.NEW.FDB
   ```

5. **Rename and bring the application back up:**

   ```
   SCHERER_001.FDB      ->  SCHERER_001.FDB.pre_numeric
   SCHERER_001.NEW.FDB  ->  SCHERER_001.FDB
   ```

### Rehearsal executed on 2026-08-04

The whole window was run end to end on the development server's `SCHERER_001`
(18.56 GB, NVMe), with the new library already installed. Steps 1 to 4 first,
with the rename deliberately deferred, so the restored copy could be checked
with the old database still beside it; step 5 afterwards, once all of section 5
had been checked.

| stage | duration | result |
|---|---|---|
| `gbak -b -PAR 5` | 2.2 min | 8.28 GB of `.fbk`, zero errors in the log |
| `gbak -c -PAR 5` | 7.3 min | 18.68 GB, zero errors in the log |
| `gfix -v -full` | 5.0 min | empty output, no page or index error |
| **total** | **~15 min** | |

The customer's window is larger than that: add the time to stop the
application, the library swap with the service stopped, and the rename. The
`gfix` is optional and can be moved out of the window, since the restore's `-v`
reported nothing.

The rename in step 5 did not need the service stopped. With the application out,
the only connections left in `MON$ATTACHMENTS` are the engine's internal ones
(`Garbage Collector` and `Cache Writer`), and in that state Windows allows both
files to be renamed. Worth checking first:

```sql
SELECT COUNT(*) FROM MON$ATTACHMENTS WHERE MON$ATTACHMENT_ID <> CURRENT_CONNECTION;
```

The `Stop-Service` of the library swap, on the other hand, does require an
elevated prompt: without elevation it fails with "cannot open service", which
does not look like a permission error but is one.

After the rename, all of section 5 was run again, now against
`D:\u\banco\scherer\SCHERER_001.FDB`, the official path. Every value came out
equal to the restored copy's.

Before starting, it is worth running the cheap gate from section 3 on the
source, forcing a scan, because a silent duplicate written during the window
only shows up as a failure in the middle of the restore, after the whole backup
has already been paid for:

```sql
SELECT COUNT(*), COUNT(DISTINCT CNPJ || '') FROM ENTIDADE;   -- 544149 / 544149
```

In the rehearsal it came out clean, even with the new library installed since
2026-08-03 08:02 and the database taking writes after that.

## 5. Validation after the restore

The baseline was re-checked on 2026-08-03 against the current `SCHERER_001`, and
all of it checked again on 2026-08-04 against the rehearsal's restored copy. The
same numbers have to come out of the restored database:

| check | value |
|---|---|
| user `RDB$RELATIONS` | 1242 (1196 tables + 46 views) |
| `RDB$PROCEDURES` | 885 |
| user `RDB$TRIGGERS` | 1812 |
| user `RDB$INDICES` | 2231 |
| inactive indexes | 7 (already so before the domain conversion) |
| triggers with `RDB$VALID_BLR = 0` | 2 (same) |
| columns on the `TDR_CNPJ` domain | 368 |
| expression indexes | 38 |
| `ENTIDADE`: rows / distinct CNPJ | 544149 / 544149 |
| `ENTIDADE`: `SUM(CAST(CNPJ AS BIGINT))` | 6151677092957447095 |
| `ENTIDADE`: `MAX(CHAR_LENGTH(CNPJ))` | 15 |

The `SUM` needs the explicit `CAST`: the column has been `VARCHAR(20)` since the
domain conversion, and `SUM` over text is refused in dialect 3
(`Argument for SUM in dialect 3 must be numeric`). The conversion runbook records
that total without the cast because the column was still numeric back then.

**Index against scan.** Each pair has to return the same number. That is what
proves the indexes were rebuilt:

```sql
SELECT COUNT(*) FROM ENTIDADE WHERE CNPJ = '06056181000154';        -- uses the index
SELECT COUNT(*) FROM ENTIDADE WHERE CNPJ || '' = '06056181000154';  -- forces a scan

SELECT COUNT(*) FROM ENTIDADE
 WHERE CNPJ BETWEEN '10000000000000' AND '99999999999999';
SELECT COUNT(*) FROM ENTIDADE
 WHERE CNPJ || '' BETWEEN '10000000000000' AND '99999999999999';    -- expects 139433
```

**The new order.** Both lists have to come out identical, and in numeric order
(`1, 2, 3, 5, 6, 8, 9, 10, 12, ...`, not `1, 10, 12, ..., 2, 3`):

```sql
SELECT CNPJ FROM ENTIDADE PLAN (ENTIDADE ORDER PK_ENTIDADE) ORDER BY CNPJ ROWS 12;
SELECT CNPJ FROM ENTIDADE PLAN (ENTIDADE NATURAL)           ORDER BY CNPJ ROWS 12;
```

Before the rebuild the first one returns zero rows, as recorded in section 1.

**A range does not reach another length.** With ordering by length, a range
between two 14 digit values cannot bring in a value of a different normalized
length, so this has to return 0:

```sql
SELECT COUNT(*) FROM ENTIDADE
 WHERE CNPJ BETWEEN '10000000000000' AND '99999999999999'
   AND CHAR_LENGTH(TRIM(LEADING '0' FROM CNPJ)) <> 14;
```

This test is only worth anything once the `BETWEEN` pair above is matching:
while the index is still old it returns 0 for the wrong reason, because the
range returns no row at all.

**Views.** The three views exposing columns of the domain (`VCONTAS_A_PAGAR`,
`VCONTAS_A_RECEBER`, `V$PRODUTO_ESTOQUE_ANALISE_01`) were already used as a
check in the domain conversion and serve again: they have to answer without
error, which shows the stored BLR is still valid. Two of them return zero rows,
both on the source and on the restored copy. That is the state of the database,
not a failed check: what is being measured is execution, not content.

**Inventory.** Run `id_zpad_ci_rollout_inventory.sql` again on the restored copy
and compare with the source's. In the 2026-08-04 rehearsal both outputs came out
byte for byte identical, 7909 lines: the 368 columns, the 421 segments and the
38 expression indexes crossed the restore unchanged.

### Result of the 2026-08-04 rehearsal

Every pair section 1 recorded as broken started matching:

| check | source (old index, new library) | restored copy |
|---|---|---|
| `CNPJ = '06056181000154'`, index / scan | **0** / 1 | 1 / 1 |
| 14 digit `BETWEEN`, index / scan | **0** / 139433 | 139433 / 139433 |
| `ORDER BY CNPJ` navigating the index | **0** of 544149 | 544149 |
| first 12 by `ORDER BY`, index and scan | - | `1,2,3,5,6,8,9,10,12,13,15,16` on both |
| range reaching another normalized length | - | 0 |
| `TDR_CNPJ` domain (type / bytes / chars / collation) | - | 37 / 20 / 20 / `ISO8859_1_LTRIM_ZERO` |

The last row records what was observed at the time, under the old collation
name. After a migration performed with the current module, the same check
returns `ISO8859_1_ID_ZPAD_CI`.

Metadata, counts and the `SUM` from the table above came out equal to the
baseline on both databases.

**Outside `ENTIDADE` and outside the simple ascending case.** All the checks
above go through `ENTIDADE` and its PRIMARY KEY, which is a single index of 421
segments. Walking an index end to end and counting what comes out is the test
that needs no sample value: had a single key been built out of order, the walk
would skip a row, which is exactly what the whole index was doing before the
rebuild. Compared against the natural count of the same table:

| index | type | navigation x natural |
|---|---|---|
| `ENTIDADE_IDX7` | descending, 1 segment | 544149 / 544149 |
| `ENTIDADE_IDX5` | descending, compound, CNPJ at position 0 | 544149 / 544149 |
| `ENTIDADE_COBRANCA_PK` | PRIMARY KEY of another table | 544570 / 544570 |
| `ENTIDADE_OBSERVACAO_IDX1` | secondary | 641273 / 641273 |
| `PRODUTO_IDX19` | plain, not unique, over `FORNECEDOR` | 144655 / 144655 |

The descending ones matter because they are the structural variant the PK does
not exercise and because `btr` adds a byte for them. The first 12 rows of
`ORDER BY CNPJ DESC` come out identical through the index and through the scan,
headed by a 15 digit value: length decides on the way down too.

Picking the indexes by `RDB$STATISTICS` avoids wasting time: statistics are
recomputed by the restore, so a segment over an empty table sits at 0 and proves
nothing. The first two candidates tried, `CONTAS_A_RECEBER` and
`CONTAS_A_PAGAR`, were empty.

## 6. `gfix -v` during the window

Between the library swap and the end of the restore, `gfix -v` reports index
corruption. That is expected: the validator reads the keys with the new driver.
It does not indicate a new problem and is not a reason to abort.

After the restore it comes out clean. In the 2026-08-04 rehearsal,
`gfix -v -full -user SYSDBA -password masterkey` over the restored copy ran in 5
minutes and did not print a single line.

## 7. Rollback

As long as nobody has written to the new database, it is reversible with no
loss:

```
SCHERER_001.FDB              ->  SCHERER_001.FDB.pos_numeric
SCHERER_001.FDB.pre_numeric  ->  SCHERER_001.FDB
```

and restore the library:

```powershell
Stop-Service FirebirdServerDefaultInstance
Copy-Item "$intl\lrsintl.dll.bak" "$intl\lrsintl.dll" -Force
Start-Service FirebirdServerDefaultInstance
```

The two steps go together: the old database with the new library is the
wrong-result state from section 1.

Once the application starts writing to the new database, going back costs those
writes. The `.fbk` from step 2 rebuilds the previous state from scratch.

## 8. Permanent effects to communicate to the customer

- **`STARTING WITH` and `LIKE 'x%'` over an indexed column start scanning the
  whole index.** The result stays correct and no row is lost (asserts 8.6, 8.6b,
  8.6c and 8.7 of the suite), but the plan gets worse, and the optimizer still
  estimates the range as selective through `REDUCE_SELECTIVITY_FACTOR_STARTING`
  (`src/jrd/optimizer/Retrieval.cpp:990`), so it may pick that index believing
  it filters. The detail is in [README.id_zpad_ci.md](README.id_zpad_ci.md).
- **`BETWEEN` changes meaning.** For fixed width CNPJ the change is what is
  wanted, but wherever the column mixes values of different lengths every range
  is worth reviewing: the range now goes by length first.
- **A report that relied on alphabetical order changes.** `ORDER BY` over those
  columns starts coming out in numeric order.
- Anyone who wants the old order in a specific report can ask for
  `ORDER BY col COLLATE ISO8859_1`, which does not go through the driver.

## 9. Other databases on the server

The library is shared by the whole installation: swapping the module changes the
behaviour of **every** database on that server that uses the collation, not only
the target one. Run the inventory on each of them and include in the same window
every database whose query 1 does not come back empty.

On the development server, of the 16 databases in `D:\u\banco\scherer` only
`SCHERER_001` uses the collation. The customer's server has to be inventoried
again, that conclusion cannot be reused.

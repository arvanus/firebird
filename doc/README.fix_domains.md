# gbak -FIX_DOMAINS: redefining domains during restore

`-FIX_DOMAINS` lets a restore change the type of a domain while the backup is being
restored, so that the columns using that domain land in the new database already with
the new type and with their data converted by the engine.

The motivating case is a numeric domain that should have been text: a `NUMERIC(18,0)`
that holds document numbers, where a leading zero is significant. Converting it after
the restore means a second full pass over the data, and doing it by hand in the backup
file is not an option. With `-FIX_DOMAINS` the conversion happens once, during the
restore that would run anyway.

The switch is accepted on restore (`-c` and `-rep`) only, never on backup. Without it,
gbak behaves exactly as the official one.

## Usage

```
gbak -c backup.fbk new.fdb -FIX_DOMAINS "TDR_CNPJ = VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE MY_COLL"
gbak -c backup.fbk new.fdb -FIX_DOMAINS @rules.conf
```

The argument is either the rules themselves, separated by `;`, or `@` followed by the
path of a file holding one rule per line. The inline form exists so that no file has to
be present on the server. The minimum abbreviation is `-FIX_D`, which already
distinguishes it from `-FIX_FSS_DATA` and `-FIX_FSS_METADATA`.

## Rule format

```
# comment until the end of the line
<DOMAIN_NAME> = VARCHAR(<n>) [CHARACTER SET <name>] [COLLATE <name>] [UNCHECKED]
<DOMAIN_NAME> = CHAR(<n>)    [CHARACTER SET <name>] [COLLATE <name>] [UNCHECKED]
```

Only `CHAR` and `VARCHAR` are accepted as target types. Character sets and collations
are always given by name, never by number: the ids differ from database to database,
and a user collation coming from the backup only gets its id while the restore runs.

- `n` is a number of characters. The byte length recorded in `RDB$FIELD_LENGTH` is
  derived from `n` and from the bytes per character of the resolved character set, so
  `VARCHAR(20) CHARACTER SET UTF8` becomes 80 bytes and 20 characters.
- `COLLATE` without `CHARACTER SET` uses the character set the collation belongs to.
- Neither clause given means the default character set of the database being created,
  which is what the DDL would give a column.
- A rule naming a collation that belongs to another character set than the one in the
  same rule is refused.
- The same domain may not appear in more than one rule.

Example rules file:

```
# document numbers, text with a significant leading zero
TDR_CNPJ = VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE ISO8859_1_LTRIM_ZERO
TDR_CPF  = VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE ISO8859_1_LTRIM_ZERO
```

## Minimum width

`n` has to be at least what `ALTER DOMAIN ... TYPE` would require for the source type,
the same rule the DDL enforces in `AlterDomainNode::checkUpdate`. For `BIGINT` that is
20 characters, and for a text source it is the character length it already has. A rule
asking for less is refused:

```
new size for domain TDR_CNPJ must be at least 20 characters, or add UNCHECKED
```

Limiting the content to fewer characters than the type allows is a job for a constraint,
not for a narrower type:

```sql
ALTER DOMAIN TDR_CNPJ ADD CONSTRAINT CHECK (CHAR_LENGTH(VALUE) <= 17);
```

A width below the minimum is a foreign key problem for nobody: an FK between a
`VARCHAR(17)` column and a `VARCHAR(20)` column of the same character set and collation
is created and works, because what the index compares is the type, not the length.

### UNCHECKED

gbak writes `RDB$FIELDS` directly and never calls `checkUpdate`, so the minimum width is
a safeguard of this feature rather than a restriction inherited from the DDL path.
`UNCHECKED` at the end of a rule turns it off for that rule, and gbak prints which
domain, which width and which minimum was overridden.

Two consequences worth knowing before using it:

- The resulting domain is a state that `ALTER DOMAIN` would refuse.
- Truncation is found late. A value that does not fit fails while the data is being
  loaded, with a conversion error from the engine, not while the rules are validated.
  On a large backup that means losing hours of work.

## Preview

There is no dry run switch. The `-m` (metadata only) that already exists does the job:

```
gbak -m -c backup.fbk dry.fdb -FIX_DOMAINS @rules.conf -v
```

It applies the remap for real, prints the resolved domains, and stops before the data.
It validates exactly the same conditions as a full restore: domain present in the
backup, character set and collation resolvable, exactly one row matched in
`RDB$FIELDS`. The resulting database is disposable.

## Output

With `-v`, one line per remapped domain plus a summary:

```
gbak:remapping domain TDR_CNPJ to VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE MY_COLL
gbak:remapped 1 domain(s), 1 rule(s) given
```

Independently of `-v`, at the end of the restore gbak lists the columns that use a
remapped domain but carry a collation of their own:

```
gbak:column T3.V keeps its own collation and does not follow domain TDR_CNPJ
```

Those columns keep their own collation, because the record format gives the column
precedence over the domain. The remap of the type still reaches them; only the
collation does not.

## Errors

Every condition below aborts the restore, and the target database is left unusable:

| Condition | Message |
|---|---|
| Rule that does not parse | `invalid domain remap rules: @1` |
| Domain named in a rule not present in the backup | `domain @1 from remap rules was not found in the backup` |
| Character set or collation name that does not resolve | `cannot resolve @1 @2 for domain @3` |
| Collation from another character set than the rule's | `invalid domain remap rules: collation ...` |
| Byte length over the column limit | `invalid domain remap rules: domain ... over the ... byte limit` |
| Domain already of the target type | `invalid domain remap rules: domain ... is already in the target type` |
| Rule matching a number of rows in `RDB$FIELDS` other than one | `invalid domain remap rules: domain ... matched @1 rows` |

Failing on a rule that matched nothing is deliberate. A rule that silently does nothing
would ship a database whose domain still has the old type, and nobody would notice until
the application did.

## Known limits

- Services API is not wired: the switch works from the command line only.
- Columns with their own collation do not follow the collation of the rule, as described
  above. They are reported, not fixed.
- A rule with `UNCHECKED` can only fail late, while the data is loading.

The design, the alternatives that were discarded and the verification that backs all of
this are in `docs/superpowers/specs/2026-08-01-gbak-domain-remap-design.md`.

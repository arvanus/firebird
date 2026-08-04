/*
 * ID_ZPAD_CI rollout inventory.
 *
 * Run with isql against every database on the server before swapping
 * lrsintl.dll. Everything is resolved through
 * RDB$COLLATIONS.RDB$BASE_COLLATION_NAME, which holds the name given to
 * CREATE COLLATION ... FROM EXTERNAL (src/dsql/DdlNodes.epp:3977), never
 * through the local collation name: the DBA is free to call it anything.
 *
 * Every filter matches the old external names (*_LTRIM_ZERO) as well as the
 * current ones. This is an inventory of a database that has NOT been migrated
 * yet, and such a database still carries the name the collation shipped under
 * before the rename. Matching only the current names would report zero rows on
 * exactly the databases this script exists to find.
 */

SET LIST ON;

/* ---------------------------------------------------------------- */
/* 1. Collations in this database that come from the ID_ZPAD_CI      */
/*    module, whatever local name they were given.                   */
/* ---------------------------------------------------------------- */

SELECT TRIM(C.RDB$COLLATION_NAME)      AS LOCAL_NAME,
       TRIM(C.RDB$BASE_COLLATION_NAME) AS EXTERNAL_NAME,
       C.RDB$COLLATION_ID              AS COLL_ID,
       C.RDB$CHARACTER_SET_ID          AS CS_ID,
       C.RDB$COLLATION_ATTRIBUTES      AS ATTRS
FROM RDB$COLLATIONS C
WHERE TRIM(COALESCE(C.RDB$BASE_COLLATION_NAME, C.RDB$COLLATION_NAME))
      IN ('ISO8859_1_ID_ZPAD_CI', 'WIN1252_ID_ZPAD_CI',
           'ISO8859_1_LTRIM_ZERO', 'WIN1252_LTRIM_ZERO');

/* ---------------------------------------------------------------- */
/* 2. Columns using one of those collations.                         */
/*    The effective collation of a column is the override in         */
/*    RDB$RELATION_FIELDS when present, otherwise the domain's in    */
/*    RDB$FIELDS. Collation ids are only unique inside a charset, so */
/*    the join carries both.                                         */
/* ---------------------------------------------------------------- */

WITH IDZ AS (
    SELECT C.RDB$COLLATION_ID AS COLL_ID,
           C.RDB$CHARACTER_SET_ID AS CS_ID
    FROM RDB$COLLATIONS C
    WHERE TRIM(COALESCE(C.RDB$BASE_COLLATION_NAME, C.RDB$COLLATION_NAME))
          IN ('ISO8859_1_ID_ZPAD_CI', 'WIN1252_ID_ZPAD_CI',
           'ISO8859_1_LTRIM_ZERO', 'WIN1252_LTRIM_ZERO')
)
SELECT TRIM(RF.RDB$RELATION_NAME) AS REL,
       TRIM(RF.RDB$FIELD_NAME)    AS FLD,
       TRIM(RF.RDB$FIELD_SOURCE)  AS DOMAIN_NAME,
       F.RDB$FIELD_LENGTH         AS BYTES
FROM RDB$RELATION_FIELDS RF
JOIN RDB$FIELDS F ON F.RDB$FIELD_NAME = RF.RDB$FIELD_SOURCE
JOIN IDZ ON IDZ.CS_ID = F.RDB$CHARACTER_SET_ID
        AND IDZ.COLL_ID = COALESCE(RF.RDB$COLLATION_ID, F.RDB$COLLATION_ID, 0)
ORDER BY 1, 2;

/* ---------------------------------------------------------------- */
/* 3. Index segments over those columns. Every one of these holds    */
/*    keys in the old format and is wrong until rebuilt.             */
/*    RDB$RELATION_CONSTRAINTS tells which ones cannot be            */
/*    deactivated with ALTER INDEX (trig.h:1377-1378).               */
/* ---------------------------------------------------------------- */

WITH IDZ AS (
    SELECT C.RDB$COLLATION_ID AS COLL_ID,
           C.RDB$CHARACTER_SET_ID AS CS_ID
    FROM RDB$COLLATIONS C
    WHERE TRIM(COALESCE(C.RDB$BASE_COLLATION_NAME, C.RDB$COLLATION_NAME))
          IN ('ISO8859_1_ID_ZPAD_CI', 'WIN1252_ID_ZPAD_CI',
           'ISO8859_1_LTRIM_ZERO', 'WIN1252_LTRIM_ZERO')
),
COLS AS (
    SELECT RF.RDB$RELATION_NAME AS REL, RF.RDB$FIELD_NAME AS FLD
    FROM RDB$RELATION_FIELDS RF
    JOIN RDB$FIELDS F ON F.RDB$FIELD_NAME = RF.RDB$FIELD_SOURCE
    JOIN IDZ ON IDZ.CS_ID = F.RDB$CHARACTER_SET_ID
            AND IDZ.COLL_ID = COALESCE(RF.RDB$COLLATION_ID, F.RDB$COLLATION_ID, 0)
)
SELECT TRIM(I.RDB$INDEX_NAME)                  AS IDX,
       TRIM(I.RDB$RELATION_NAME)               AS REL,
       TRIM(S.RDB$FIELD_NAME)                  AS SEG,
       S.RDB$FIELD_POSITION                    AS POS,
       COALESCE(I.RDB$UNIQUE_FLAG, 0)          AS IS_UNIQUE,
       COALESCE(I.RDB$INDEX_TYPE, 0)           AS IS_DESC,
       COALESCE(I.RDB$INDEX_INACTIVE, 0)       AS INACTIVE,
       TRIM(COALESCE(RC.RDB$CONSTRAINT_TYPE, 'NONE')) AS CONSTRAINT_TYPE
FROM RDB$INDEX_SEGMENTS S
JOIN RDB$INDICES I ON I.RDB$INDEX_NAME = S.RDB$INDEX_NAME
JOIN COLS ON COLS.REL = I.RDB$RELATION_NAME AND COLS.FLD = S.RDB$FIELD_NAME
LEFT JOIN RDB$RELATION_CONSTRAINTS RC ON RC.RDB$INDEX_NAME = I.RDB$INDEX_NAME
ORDER BY 2, 1, 4;

/* ---------------------------------------------------------------- */
/* 4. Expression indexes. Their result can carry the collation, and  */
/*    an expression index stores no rows in RDB$INDEX_SEGMENTS       */
/*    (verified on SCHERER_001: 38 expression indexes, 0 segment     */
/*    rows), so query 3 cannot see them. The engine keys them from   */
/*    the expression result through the single segment path          */
/*    (btr.cpp:2059), which means the collation of that result is    */
/*    what decides. Read the source of each one by hand and match it */
/*    against the column list from query 2.                          */
/* ---------------------------------------------------------------- */

SELECT TRIM(I.RDB$INDEX_NAME)    AS IDX,
       TRIM(I.RDB$RELATION_NAME) AS REL,
       I.RDB$EXPRESSION_SOURCE   AS EXPR
FROM RDB$INDICES I
WHERE I.RDB$EXPRESSION_BLR IS NOT NULL
ORDER BY 2, 1;

/* ---------------------------------------------------------------- */
/* 5. Key size headroom. Each segment of an affected index grows by  */
/*    2 bytes, a descending index adds 1 more, and a compound index  */
/*    amplifies that by roughly 25% of stuff bytes (btr.cpp:1727).   */
/*    The limit is page_size / 4 (Database.h:652-655).               */
/*                                                                   */
/*    This is a rough upper bound, not the exact engine computation: */
/*    anything it flags has to be looked at, anything it clears by a */
/*    wide margin is safe.                                           */
/* ---------------------------------------------------------------- */

WITH IDZ AS (
    SELECT C.RDB$COLLATION_ID AS COLL_ID,
           C.RDB$CHARACTER_SET_ID AS CS_ID
    FROM RDB$COLLATIONS C
    WHERE TRIM(COALESCE(C.RDB$BASE_COLLATION_NAME, C.RDB$COLLATION_NAME))
          IN ('ISO8859_1_ID_ZPAD_CI', 'WIN1252_ID_ZPAD_CI',
           'ISO8859_1_LTRIM_ZERO', 'WIN1252_LTRIM_ZERO')
),
SEG AS (
    SELECT I.RDB$INDEX_NAME AS IDX,
           I.RDB$RELATION_NAME AS REL,
           COALESCE(I.RDB$INDEX_TYPE, 0) AS IS_DESC,
           COUNT(*) AS SEGMENTS,
           SUM(F.RDB$FIELD_LENGTH) AS RAW_BYTES,
           SUM(CASE WHEN IDZ.COLL_ID IS NULL THEN 0 ELSE 2 END) AS EXTRA_BYTES
    FROM RDB$INDEX_SEGMENTS S
    JOIN RDB$INDICES I ON I.RDB$INDEX_NAME = S.RDB$INDEX_NAME
    JOIN RDB$RELATION_FIELDS RF ON RF.RDB$RELATION_NAME = I.RDB$RELATION_NAME
                               AND RF.RDB$FIELD_NAME = S.RDB$FIELD_NAME
    JOIN RDB$FIELDS F ON F.RDB$FIELD_NAME = RF.RDB$FIELD_SOURCE
    LEFT JOIN IDZ ON IDZ.CS_ID = F.RDB$CHARACTER_SET_ID
                 AND IDZ.COLL_ID = COALESCE(RF.RDB$COLLATION_ID, F.RDB$COLLATION_ID, 0)
    GROUP BY 1, 2, 3
)
SELECT TRIM(IDX) AS IDX, TRIM(REL) AS REL, SEGMENTS, RAW_BYTES, EXTRA_BYTES,
       CAST((RAW_BYTES + EXTRA_BYTES + IS_DESC)
            * (CASE WHEN SEGMENTS > 1 THEN 1.25 ELSE 1.0 END) AS INTEGER) AS EST_KEY_BYTES,
       (SELECT MON$PAGE_SIZE / 4 FROM MON$DATABASE) AS KEY_LIMIT
FROM SEG
WHERE EXTRA_BYTES > 0
ORDER BY 6 DESC;

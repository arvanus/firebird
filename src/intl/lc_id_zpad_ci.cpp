/*
 *	PROGRAM:	InterBase International support
 *	MODULE:		lc_id_zpad_ci.cpp
 *	DESCRIPTION:	Custom collation: LTRIM zeros and spaces, case-insensitive
 *
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 *
 * Description:
 *  Single byte collation for WIN1252 / ISO8859_1 that ignores leading '0'
 *  and ' ' characters and compares case-insensitively (ASCII range only).
 *
 *  Example: "00000A" = "0A" = "A" = "    A" = "a"
 *
 *  Every string made only of '0' and ' ' normalizes to the empty string,
 *  so "000" = "   " = " 0 " = "".
 *
 *  When the collation is declared PAD SPACE, trailing spaces are ignored
 *  as well, which is what makes CHAR columns behave: 'A' stored in CHAR(5)
 *  is physically "A    " and must still compare equal to the literal 'A'.
 *
 *  Implementation notes:
 *
 *  - compare() and string_to_key() share the same normalization primitive
 *    (normalize_bounds) on purpose, and both order by the LENGTH of the
 *    normalized form before its bytes. The sort key is that length as two
 *    big endian bytes followed by the normalized string in upper case, so a
 *    byte comparison of two keys reproduces compare() exactly, which is what
 *    the b-tree relies on. Keep them in sync: if they ever diverge, UNIQUE
 *    constraints and DISTINCT stop agreeing with '='. For the same reason
 *    TEXTTYPE_SEPARATE_UNIQUE is not needed.
 *
 *  - Ordering by length first is what makes the collation behave numerically
 *    for values that are digit strings of different widths: '9' sorts before
 *    '0001A34', and BETWEEN over a range of 14 digit values does not swallow
 *    an 11 digit one. Equality is unchanged: '000123' still equals '123'.
 *
 *  - INTL_KEY_PARTIAL returns an empty key, because with the length in front
 *    the key of a prefix is not a prefix of the key of the value. The engine
 *    turns an empty starting key into a full index scan and re-checks
 *    blr_starting against the record, so STARTING WITH and LIKE 'x%' stay
 *    correct and only lose the index range. See the comment inside
 *    texttype_fn_str_to_key for the exact chain.
 *
 *  - No dynamic allocation and no temporary buffers anywhere. INTL
 *    callbacks are invoked by the engine without any exception barrier
 *    (see Jrd::TextType) and run on every index key and every comparison,
 *    so they must not throw and must not touch the shared memory pool.
 *
 *  - Pattern matching (LIKE / CONTAINING / STARTING WITH / SIMILAR TO)
 *    goes through texttype_fn_canonical, which is a per-character mapping
 *    and therefore cannot express removal of leading zeros. Canonical form
 *    here is plain uppercasing, so pattern matching is case-insensitive but
 *    NOT zero-insensitive. STARTING WITH is still evaluated over the record
 *    after the index scan, so both plans return the same rows; what changes
 *    is that the index no longer narrows the range. Use '=' with this
 *    collation, or an expression index on a normalized column.
 *
 *  - str_to_upper / str_to_lower are deliberately NOT installed so that
 *    UPPER()/LOWER() keep using the engine's ICU based case folding and
 *    keep working for accented characters.
 */

#include "firebird.h"
#include "../intl/ldcommon.h"
#include "ld_proto.h"
#include <string.h>

namespace {

const UCHAR ASCII_SPACE = 32;
const UCHAR ASCII_ZERO = '0';

// Locale independent uppercase conversion (ASCII range only)
inline UCHAR ascii_toupper(UCHAR c)
{
	return (c >= 'a' && c <= 'z') ? (UCHAR) (c - 32) : c;
}

// Compute the significant range [begin, end) of a string:
// trailing spaces are dropped when the collation pads with spaces,
// then leading zeros and spaces are dropped.
// Shared by compare() and string_to_key() so both always agree.
inline void normalize_bounds(bool pad, const UCHAR* src, ULONG len,
							 const UCHAR*& begin, const UCHAR*& end)
{
	begin = src;
	end = src + len;

	if (pad)
	{
		while (end > begin && end[-1] == ASCII_SPACE)
			end--;
	}

	while (begin < end && (*begin == ASCII_ZERO || *begin == ASCII_SPACE))
		begin++;
}

} // namespace


static SSHORT texttype_fn_compare(texttype* obj,
								  ULONG len1, const UCHAR* str1,
								  ULONG len2, const UCHAR* str2,
								  INTL_BOOL* error_flag)
{
	fb_assert(str1 != NULL || len1 == 0);
	fb_assert(str2 != NULL || len2 == 0);
	fb_assert(error_flag != NULL);

	*error_flag = false;

	const bool pad = obj->texttype_pad_option != 0;

	const UCHAR* p1;
	const UCHAR* e1;
	normalize_bounds(pad, str1, len1, p1, e1);

	const UCHAR* p2;
	const UCHAR* e2;
	normalize_bounds(pad, str2, len2, p2, e2);

	// Numeric ordering: the shorter normalized form always sorts first,
	// whatever its bytes are. This is what puts '9' before '0001A34' and what
	// keeps BETWEEN over a range of same length values from swallowing a
	// shorter one. The keys built below reproduce this by carrying the length
	// in front.
	const ULONG n1 = (ULONG) (e1 - p1);
	const ULONG n2 = (ULONG) (e2 - p2);

	if (n1 != n2)
		return (n1 < n2) ? -1 : 1;

	while (p1 < e1)
	{
		const UCHAR c1 = ascii_toupper(*p1++);
		const UCHAR c2 = ascii_toupper(*p2++);

		if (c1 != c2)
			return (c1 < c2) ? -1 : 1;
	}

	return 0;
}


static USHORT texttype_fn_str_to_key(texttype* obj,
									 USHORT srcLen, const UCHAR* src,
									 USHORT dstLen, UCHAR* dst,
									 USHORT key_type)
{
	fb_assert(src != NULL || srcLen == 0);
	fb_assert(dst != NULL);

	// A partial key cannot exist in this format. The key starts with the
	// length of the WHOLE normalized string, so the key of a prefix is not a
	// byte prefix of the key of the value, and no amount of padding makes it
	// one. An empty key is the safe answer: BTR_make_key flags it as empty
	// (btr.cpp:1900), a fuzzy scan with an empty key does not stop early
	// (btr.cpp:1904-1908, 6893, 6939) and turns into a full index scan, and
	// blr_starting is re-evaluated over the record afterwards
	// (Optimizer.cpp:3008-3035), so STARTING WITH stays correct and only gets
	// slower.
	//
	// INTL_BAD_KEY_LENGTH must NOT be used for this. The return value is not
	// checked (intl.cpp:1245-1249, btr.cpp:2882) and btr.cpp:2926 truncates
	// (USHORT) -1 into the maximum key size, building a key out of
	// uninitialized buffer memory.
	if (key_type == INTL_KEY_PARTIAL)
		return 0;

	const UCHAR* p;
	const UCHAR* end;
	normalize_bounds(obj->texttype_pad_option != 0, src, srcLen, p, end);

	const USHORT len = (USHORT) (end - p);

	// [2 bytes: length of the normalized form, big endian][normalized bytes]
	//
	// Big endian so that a plain byte comparison of two keys reproduces
	// compare(): length first, content second. INTL_KEY_UNIQUE is identical to
	// INTL_KEY_SORT, because this key already defines the equality class.
	if (dstLen < 2)
	{
		fb_assert(false);	// key_length() promised at least len + 2
		return 0;
	}

	dst[0] = (UCHAR) (len >> 8);
	dst[1] = (UCHAR) (len & 0xFF);

	USHORT pos = 2;

	while (p < end && pos < dstLen)
		dst[pos++] = ascii_toupper(*p++);

	// dstLen can legitimately be smaller than len + 2, so the loop above must
	// stop on it and there is nothing to assert here. INTL_key_length caps the
	// key at MAX_KEY = 8192 and then raises it back to the raw field length
	// (intl.cpp:1013-1017), so a VARCHAR(12000) of non strippable characters
	// gets dstLen = 12000 for a key that would want 12002. That truncation
	// predates this change and is not checked by the caller
	// (SortedStream.cpp:265). The length prefix stays truthful, so two values
	// of different normalized length still get different keys even when both
	// bodies are cut at the same point, which is strictly better than the old
	// format, where truncation lost the distinction entirely.

	// Do not pad the rest of dst. On the index path the engine passes
	// dstLen = 32767 no matter how long the value is, and it uses only the
	// returned length. Padding would memset 32 KB per key.

	return pos;
}


static USHORT texttype_fn_key_length(texttype* /*obj*/, USHORT len)
{
	// Normalization only removes characters, so the normalized part never
	// exceeds len. The two extra bytes are the big endian length prefix
	// written by texttype_fn_str_to_key. len comes from a column width, capped
	// by MAX_COLUMN_SIZE (32767), so len + 2 cannot wrap a USHORT.
	return len + 2;
}


static ULONG texttype_fn_canonical(texttype* /*obj*/,
								   ULONG srcLen, const UCHAR* src,
								   ULONG dstLen, UCHAR* dst)
{
	fb_assert(src != NULL || srcLen == 0);
	fb_assert(dst != NULL);
	fb_assert(dstLen >= srcLen);

	// Exactly texttype_canonical_width (1) byte per character, returning the
	// number of characters processed. No trimming is possible here: this is a
	// per character mapping, and the engine also feeds it single characters
	// (including the space) when building Jrd::TextType::canonicalChars.
	const ULONG count = MIN(srcLen, dstLen);

	for (ULONG i = 0; i < count; i++)
		dst[i] = ascii_toupper(src[i]);

	return count;
}


TEXTTYPE_ENTRY3(LCIDZPADCI_init)
{
	// The collation is always case-insensitive over the ASCII range, so
	// TEXTTYPE_ATTR_CASE_INSENSITIVE changes nothing here. It is accepted
	// anyway because SIMILAR TO does not use texttype_fn_canonical: it builds
	// an RE2 pattern and reads case insensitivity straight from the collation
	// attributes (Jrd::Collation.cpp, Re2SimilarMatcher). Declaring
	// CASE INSENSITIVE in CREATE COLLATION is what keeps SIMILAR TO in line
	// with LIKE and CONTAINING.
	//
	// ACCENT INSENSITIVE is rejected: comparison is ASCII only, so accepting
	// it would make SIMILAR TO fold accents while '=' does not.
	if ((attributes & ~(TEXTTYPE_ATTR_PAD_SPACE | TEXTTYPE_ATTR_CASE_INSENSITIVE)) ||
		specific_attributes_length)
	{
		return false;
	}

	cache->texttype_version = TEXTTYPE_VERSION_1;
	cache->texttype_name = "ID_ZPAD_CI";
	cache->texttype_country = CC_INTL;
	cache->texttype_pad_option = (attributes & TEXTTYPE_ATTR_PAD_SPACE) ? true : false;

	// Canonical form is uppercase, one byte per character. Width and function
	// must be set together, see the assertion in INTL_texttype_lookup.
	cache->texttype_canonical_width = 1;
	cache->texttype_fn_canonical = texttype_fn_canonical;

	// No TEXTTYPE_DIRECT_MATCH: the canonical form differs from the raw bytes.
	// No TEXTTYPE_SEPARATE_UNIQUE: the sort key already defines equality.
	cache->texttype_flags = 0;

	cache->texttype_fn_key_length = texttype_fn_key_length;
	cache->texttype_fn_string_to_key = texttype_fn_str_to_key;
	cache->texttype_fn_compare = texttype_fn_compare;

	// texttype_fn_str_to_upper / _str_to_lower intentionally left unset so
	// UPPER() and LOWER() keep the full charset case mapping through ICU.

	// The collation is stateless, so there is nothing to store and nothing
	// to release: texttype_impl and texttype_fn_destroy stay NULL.

	return true;
}

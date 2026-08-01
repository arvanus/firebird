/*
 *	PROGRAM:	InterBase International support
 *	MODULE:		lc_ltrim_zero.cpp
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
 *    (normalize_bounds) on purpose. The sort key IS the normalized string,
 *    so byte order of the keys reproduces exactly the order returned by
 *    compare(), including the "shorter prefix sorts first" rule used by
 *    the b-tree (btr.cpp find_node_start_point). Keep them in sync: if
 *    they ever diverge, UNIQUE constraints and DISTINCT stop agreeing
 *    with '='. For the same reason TEXTTYPE_SEPARATE_UNIQUE is not needed.
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
 *    NOT zero-insensitive. As a consequence STARTING WITH may return
 *    different rows depending on whether an index is used. Use '=' with
 *    this collation, or an expression index on a normalized column.
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

	while (p1 < e1 && p2 < e2)
	{
		const UCHAR c1 = ascii_toupper(*p1++);
		const UCHAR c2 = ascii_toupper(*p2++);

		if (c1 != c2)
			return (c1 < c2) ? -1 : 1;
	}

	// Equal prefixes: the shorter normalized string sorts first.
	// This matches the byte ordering of the keys built below.
	if (p1 < e1)
		return 1;

	if (p2 < e2)
		return -1;

	return 0;
}


static USHORT texttype_fn_str_to_key(texttype* obj,
									 USHORT srcLen, const UCHAR* src,
									 USHORT dstLen, UCHAR* dst,
									 USHORT /*key_type*/)
{
	fb_assert(src != NULL || srcLen == 0);
	fb_assert(dst != NULL);

	const UCHAR* p;
	const UCHAR* end;
	normalize_bounds(obj->texttype_pad_option != 0, src, srcLen, p, end);

	// The key is the normalized string itself, for every key type.
	//
	// INTL_KEY_UNIQUE is identical to INTL_KEY_SORT because the sort key
	// already defines the equality class exactly.
	//
	// INTL_KEY_PARTIAL needs no special case either: the normalized form of
	// a prefix is always a byte prefix of the normalized form of the whole
	// string, since characters are only removed at the very beginning. A
	// prefix made only of zeros and spaces normalizes to an empty key, which
	// the engine turns into a full scan of the index (btr.cpp), so no row is
	// ever missed.

	USHORT len = 0;

	while (p < end)
	{
		if (len >= dstLen)
			return INTL_BAD_KEY_LENGTH;

		dst[len++] = ascii_toupper(*p++);
	}

	// Do not pad the rest of dst. On the index path the engine passes
	// dstLen = 32767 no matter how long the value is, and it uses only the
	// returned length. Padding would memset 32 KB per key.

	return len;
}


static USHORT texttype_fn_key_length(texttype* /*obj*/, USHORT len)
{
	// The key never grows: normalization only removes characters.
	return len;
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


TEXTTYPE_ENTRY3(LCLTRIMZERO_init)
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
	cache->texttype_name = "LTRIM_ZERO";
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

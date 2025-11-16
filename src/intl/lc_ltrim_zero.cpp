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
 *  This collation removes leading zeros and spaces before comparison.
 *  Example: "00000A" = "0A" = "A" = "    A" = "a"
 *  Case-insensitive comparison using locale-independent uppercase conversion.
 */

#include "firebird.h"
#include "../common/classes/alloc.h"
#include "../intl/ldcommon.h"
#include "ld_proto.h"
#include <string.h>

using namespace Firebird;

namespace {

// Empty implementation structure - we don't need internal state
struct TextTypeImpl
{
};

} // namespace

// Constants for buffer management
const ULONG STACK_BUFFER_SIZE = 1024;  // Use stack for small strings
const ULONG MAX_SAFE_STRING = 32000;   // Safety limit

// Locale-independent character conversion (ASCII only)
static inline UCHAR ascii_toupper(UCHAR c)
{
	return (c >= 'a' && c <= 'z') ? (c - 32) : c;
}

static inline UCHAR ascii_tolower(UCHAR c)
{
	return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

// String normalization: remove leading zeros/spaces and convert to uppercase
static ULONG normalize_string(const UCHAR* input, ULONG input_len,
                              UCHAR* output, ULONG max_output_len)
{
	if (!input || !output || input_len == 0 || max_output_len == 0)
		return 0;

	const UCHAR* p = input;
	const UCHAR* end = input + input_len;
	UCHAR* out = output;
	const UCHAR* out_end = output + max_output_len;

	// Skip leading zeros and spaces
	while (p < end && (*p == '0' || *p == ' '))
		p++;

	// If entire string was zeros/spaces, keep the last character
	if (p >= end) {
		if (out < out_end) {
			*out++ = ascii_toupper(input[input_len - 1]);
			return 1;
		}
		return 0;
	}

	// Copy remaining characters in uppercase with bounds checking
	while (p < end && out < out_end) {
		*out++ = ascii_toupper(*p);
		p++;
	}

	return (ULONG)(out - output);
}

// Compare function
static SSHORT texttype_fn_compare(texttype* /*obj*/,
                                  ULONG len1, const UCHAR* str1,
                                  ULONG len2, const UCHAR* str2,
                                  INTL_BOOL* error_flag)
{
	// Validate inputs
	if (!error_flag)
		return 0;

	*error_flag = 0;

	if (!str1 || !str2) {
		*error_flag = 1;
		return 0;
	}

	// Validate lengths
	if (len1 > MAX_SAFE_STRING || len2 > MAX_SAFE_STRING) {
		*error_flag = 1;
		return 0;
	}

	// Use stack for small strings, heap for large
	UCHAR stack_norm1[STACK_BUFFER_SIZE];
	UCHAR stack_norm2[STACK_BUFFER_SIZE];

	UCHAR* norm1 = (len1 <= STACK_BUFFER_SIZE) ? stack_norm1 : FB_NEW_POOL(*getDefaultMemoryPool()) UCHAR[len1];
	UCHAR* norm2 = (len2 <= STACK_BUFFER_SIZE) ? stack_norm2 : FB_NEW_POOL(*getDefaultMemoryPool()) UCHAR[len2];

	if (!norm1 || !norm2) {
		if (norm1 && norm1 != stack_norm1)
			delete[] norm1;
		if (norm2 && norm2 != stack_norm2)
			delete[] norm2;
		*error_flag = 1;
		return 0;
	}

	// Normalize both strings
	ULONG norm_len1 = normalize_string(str1, len1, norm1, len1);
	ULONG norm_len2 = normalize_string(str2, len2, norm2, len2);

	// Compare up to minimum length
	const ULONG min_len = (norm_len1 < norm_len2) ? norm_len1 : norm_len2;
	int result = 0;

	if (min_len > 0)
		result = memcmp(norm1, norm2, min_len);

	// Cleanup heap allocations
	if (norm1 != stack_norm1)
		delete[] norm1;
	if (norm2 != stack_norm2)
		delete[] norm2;

	// Return comparison result
	if (result != 0)
		return (result < 0) ? -1 : 1;

	// If prefixes are equal, shorter string comes first
	if (norm_len1 < norm_len2)
		return -1;
	if (norm_len1 > norm_len2)
		return 1;

	return 0;
}

// Generate sort key for index
static USHORT texttype_fn_str_to_key(texttype* /*obj*/,
                                     USHORT srcLen, const UCHAR* src,
                                     USHORT dstLen, UCHAR* dst,
                                     USHORT /*key_type*/)
{
	if (!src || !dst || srcLen == 0 || dstLen == 0)
		return 0;

	if (srcLen > MAX_SAFE_STRING)
		return INTL_BAD_KEY_LENGTH;

	// Use stack buffer for reasonable sizes, heap for large
	UCHAR stack_buf[STACK_BUFFER_SIZE];
	UCHAR* normalized = (srcLen <= STACK_BUFFER_SIZE) ? stack_buf : FB_NEW_POOL(*getDefaultMemoryPool()) UCHAR[srcLen];

	if (!normalized)
		return INTL_BAD_KEY_LENGTH;

	// Normalize the string
	ULONG norm_len = normalize_string(src, srcLen, normalized, srcLen);

	// Check if key fits in destination
	if (norm_len > dstLen) {
		if (normalized != stack_buf)
			delete[] normalized;
		return INTL_BAD_KEY_LENGTH;
	}

	// Copy to destination buffer
	if (norm_len > 0)
		memcpy(dst, normalized, norm_len);

	// Pad with zeros if needed
	if (norm_len < dstLen)
		memset(dst + norm_len, 0, dstLen - norm_len);

	// Cleanup
	if (normalized != stack_buf)
		delete[] normalized;

	return (USHORT)norm_len;
}

// Calculate key length
static USHORT texttype_fn_key_length(texttype* /*obj*/, USHORT len)
{
	// Worst case: no leading zeros/spaces removed
	return len;
}

// Convert string to uppercase
static ULONG texttype_fn_to_upper(texttype* /*obj*/,
                                  ULONG srcLen, const UCHAR* src,
                                  ULONG dstLen, UCHAR* dst)
{
	if (!src || !dst)
		return 0;

	const ULONG copy_len = (srcLen < dstLen) ? srcLen : dstLen;

	for (ULONG i = 0; i < copy_len; i++)
		dst[i] = ascii_toupper(src[i]);

	return copy_len;
}

// Convert string to lowercase
static ULONG texttype_fn_to_lower(texttype* /*obj*/,
                                  ULONG srcLen, const UCHAR* src,
                                  ULONG dstLen, UCHAR* dst)
{
	if (!src || !dst)
		return 0;

	const ULONG copy_len = (srcLen < dstLen) ? srcLen : dstLen;

	for (ULONG i = 0; i < copy_len; i++)
		dst[i] = ascii_tolower(src[i]);

	return copy_len;
}

// Destroy function - cleanup allocated resources
static void texttype_fn_destroy(texttype* tt)
{
	if (tt && tt->texttype_impl) {
		TextTypeImpl* impl = static_cast<TextTypeImpl*>(tt->texttype_impl);
		delete impl;
		tt->texttype_impl = nullptr;
	}
}

// Initialization function using Firebird's TEXTTYPE_ENTRY macro
TEXTTYPE_ENTRY(LCLTRIMZERO_init)
{
	// Only accept PAD SPACE attribute
	if ((attributes & ~TEXTTYPE_ATTR_PAD_SPACE) || specific_attributes_length)
		return false;

	// Create implementation structure
	TextTypeImpl* impl = FB_NEW TextTypeImpl;

	// Initialize texttype structure
	cache->texttype_version = TEXTTYPE_VERSION_1;
	cache->texttype_name = "LTRIM_ZERO";
	cache->texttype_country = CC_INTL;
	cache->texttype_pad_option = (attributes & TEXTTYPE_ATTR_PAD_SPACE) ? true : false;
	cache->texttype_canonical_width = 1;  // Single byte
	cache->texttype_flags = 0;

	// Set function pointers
	cache->texttype_fn_key_length = texttype_fn_key_length;
	cache->texttype_fn_string_to_key = texttype_fn_str_to_key;
	cache->texttype_fn_compare = texttype_fn_compare;
	cache->texttype_fn_str_to_upper = texttype_fn_to_upper;
	cache->texttype_fn_str_to_lower = texttype_fn_to_lower;
	cache->texttype_fn_destroy = texttype_fn_destroy;

	// Set implementation
	cache->texttype_impl = impl;

	return true;
}

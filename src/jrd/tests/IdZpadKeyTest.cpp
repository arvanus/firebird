/*
 *	PROGRAM:	JRD engine tests
 *	MODULE:		IdZpadKeyTest.cpp
 *	DESCRIPTION:	Unit tests for the ID_ZPAD_CI ordering and sort key
 *
 * These tests call the collation driver directly, in process, with no server
 * and no database. They own the one invariant everything else rests on: the
 * sign of compare() must equal the sign of a plain byte comparison of the two
 * sort keys. If those ever disagree, an index stops describing the order the
 * engine believes it describes, and UNIQUE, DISTINCT and BETWEEN all start
 * lying. IdZpadCollationTest.cpp checks the same property through a real
 * b-tree, which is slower and much harder to read when it breaks.
 *
 * Run only this suite with:
 *     engine_test --run_test=IntlSuite/IdZpadKeySuite
 */

#include "firebird.h"
#include "boost/test/unit_test.hpp"

#include <algorithm>
#include <string.h>
#include <string>
#include <vector>

// A private copy of the driver is compiled into this test: the functions
// behind the texttype vtable are static, so they cannot be reached any other
// way, and the entry point is renamed so it can never collide with the copy
// that lives inside fbintl. src/intl/lrsintl/ld_min.cpp uses the same trick.
#define LCIDZPADCI_init LCIDZPADCI_init_under_test
#include "../../intl/lc_id_zpad_ci.cpp"
#undef LCIDZPADCI_init

namespace
{

int sign(int value)
{
	return (value > 0) - (value < 0);
}


// Byte comparison of two keys, with length as the tie breaker. This is what
// the b-tree does when it walks a page, so the test must reproduce it exactly
// rather than lean on std::string::compare.
int compareKeys(const std::string& k1, const std::string& k2)
{
	const size_t common = std::min(k1.size(), k2.size());

	if (common)
	{
		const int r = memcmp(k1.data(), k2.data(), common);

		if (r)
			return sign(r);
	}

	if (k1.size() != k2.size())
		return k1.size() < k2.size() ? -1 : 1;

	return 0;
}


// An initialized texttype plus the two calls under test.
class Driver
{
public:
	explicit Driver(USHORT attributes = TEXTTYPE_ATTR_PAD_SPACE | TEXTTYPE_ATTR_CASE_INSENSITIVE)
	{
		memset(&tt, 0, sizeof(tt));

		const INTL_BOOL ok = LCIDZPADCI_init_under_test(&tt, nullptr,
			"WIN1252_ID_ZPAD_CI", "WIN1252", attributes, nullptr, 0, nullptr);

		BOOST_REQUIRE(ok);
		BOOST_REQUIRE(tt.texttype_fn_compare != nullptr);
		BOOST_REQUIRE(tt.texttype_fn_string_to_key != nullptr);
		BOOST_REQUIRE(tt.texttype_fn_key_length != nullptr);
	}

	int compare(const std::string& a, const std::string& b)
	{
		INTL_BOOL error = true;

		const SSHORT r = tt.texttype_fn_compare(&tt,
			(ULONG) a.size(), (const UCHAR*) a.data(),
			(ULONG) b.size(), (const UCHAR*) b.data(), &error);

		BOOST_REQUIRE(!error);

		return sign(r);
	}

	std::string key(const std::string& value, USHORT keyType = INTL_KEY_SORT)
	{
		// Poisoned so that a key built out of uninitialized memory shows up as
		// 0xCC bytes instead of silently passing.
		UCHAR buffer[512];
		memset(buffer, 0xCC, sizeof(buffer));

		const USHORT len = tt.texttype_fn_string_to_key(&tt,
			(USHORT) value.size(), (const UCHAR*) value.data(),
			(USHORT) sizeof(buffer), buffer, keyType);

		BOOST_REQUIRE_MESSAGE(len != INTL_BAD_KEY_LENGTH,
			"string_to_key must never return INTL_BAD_KEY_LENGTH: the engine "
			"does not check the return value and truncates it into a key size");
		BOOST_REQUIRE(len <= sizeof(buffer));

		return std::string((const char*) buffer, len);
	}

	USHORT keyLength(USHORT len)
	{
		return tt.texttype_fn_key_length(&tt, len);
	}

	texttype tt;
};


// Equivalence classes in ascending order. Values inside a group must compare
// equal and produce byte identical keys; group i must sort before group j for
// every i < j. This is the table of section 3 of the design spec.
const std::vector<std::vector<std::string>> ORDERED_CLASSES =
{
	{ "000", "   ", "", " 0 ", "0 0" },		// normalizes to empty, length 0
	{ "9", "0000009", "  9" },				// length 1
	{ "A", "a", "00a", "  A  " },			// length 1, '9' < 'A'
	{ "10", "0010" },						// length 2
	{ "1A34", "0001A34" },					// length 4
	{ "12345678901" },						// length 11
	{ "12345678000199" },					// length 14
	{ "12345678009999" }					// length 14, byte order decides
};


std::vector<std::string> allValues()
{
	std::vector<std::string> out;

	for (const auto& group : ORDERED_CLASSES)
		out.insert(out.end(), group.begin(), group.end());

	return out;
}

} // anonymous namespace


BOOST_AUTO_TEST_SUITE(IntlSuite)
BOOST_AUTO_TEST_SUITE(IdZpadKeySuite)


// The headline behaviour: '9' sorts before '0001A34' because the normalized
// form is shorter, not because of its bytes.
BOOST_AUTO_TEST_CASE(ShorterNormalizedFormSortsFirst)
{
	Driver d;

	BOOST_CHECK_EQUAL(d.compare("9", "0001A34"), -1);
	BOOST_CHECK_EQUAL(d.compare("0000009", "0001A34"), -1);
	BOOST_CHECK_EQUAL(d.compare("10", "0001A34"), -1);
	BOOST_CHECK_EQUAL(d.compare("0001A34", "12345678901"), -1);

	// Same length: plain byte order, digits before letters.
	BOOST_CHECK_EQUAL(d.compare("9", "A"), -1);
	BOOST_CHECK_EQUAL(d.compare("A", "B"), -1);
}


BOOST_AUTO_TEST_CASE(ClassesAreTotallyOrdered)
{
	Driver d;

	for (size_t i = 0; i < ORDERED_CLASSES.size(); i++)
	{
		// Inside a class every value is equal to every other one.
		for (const std::string& a : ORDERED_CLASSES[i])
		{
			for (const std::string& b : ORDERED_CLASSES[i])
			{
				BOOST_TEST_CONTEXT("'" << a << "' vs '" << b << "'")
				{
					BOOST_CHECK_EQUAL(d.compare(a, b), 0);
					BOOST_CHECK(d.key(a) == d.key(b));
				}
			}
		}

		// Every value of an earlier class sorts before every value of a later
		// one, in both directions.
		for (size_t j = i + 1; j < ORDERED_CLASSES.size(); j++)
		{
			for (const std::string& a : ORDERED_CLASSES[i])
			{
				for (const std::string& b : ORDERED_CLASSES[j])
				{
					BOOST_TEST_CONTEXT("'" << a << "' before '" << b << "'")
					{
						BOOST_CHECK_EQUAL(d.compare(a, b), -1);
						BOOST_CHECK_EQUAL(d.compare(b, a), 1);
					}
				}
			}
		}
	}
}


// The invariant that holds the whole design together.
BOOST_AUTO_TEST_CASE(KeyOrderReproducesCompare)
{
	const std::vector<std::string> values = allValues();

	const USHORT attributeSets[] =
	{
		TEXTTYPE_ATTR_PAD_SPACE | TEXTTYPE_ATTR_CASE_INSENSITIVE,
		TEXTTYPE_ATTR_CASE_INSENSITIVE		// no PAD SPACE
	};

	for (const USHORT attributes : attributeSets)
	{
		Driver d(attributes);

		for (const std::string& a : values)
		{
			for (const std::string& b : values)
			{
				const int byCompare = d.compare(a, b);
				const int byKey = compareKeys(d.key(a), d.key(b));

				BOOST_TEST_CONTEXT("pad=" << ((attributes & TEXTTYPE_ATTR_PAD_SPACE) ? 1 : 0)
					<< " '" << a << "' vs '" << b << "'")
				{
					BOOST_CHECK_EQUAL(byCompare, byKey);
				}
			}
		}
	}
}


// With the length up front, two different keys always differ inside the first
// two bytes or have the same total length. That is exactly why the key of a
// prefix is no longer a prefix of the key of the whole value, which is what
// forces INTL_KEY_PARTIAL to give up.
BOOST_AUTO_TEST_CASE(NoKeyIsAProperPrefixOfAnother)
{
	Driver d;
	const std::vector<std::string> values = allValues();

	for (const std::string& a : values)
	{
		for (const std::string& b : values)
		{
			const std::string k1 = d.key(a);
			const std::string k2 = d.key(b);

			if (k1.size() == k2.size())
				continue;

			const std::string& shorter = k1.size() < k2.size() ? k1 : k2;
			const std::string& longer = k1.size() < k2.size() ? k2 : k1;

			BOOST_TEST_CONTEXT("'" << a << "' vs '" << b << "'")
			{
				BOOST_CHECK(memcmp(shorter.data(), longer.data(), shorter.size()) != 0);
			}
		}
	}
}


BOOST_AUTO_TEST_CASE(KeyFormatIsLengthPrefixed)
{
	Driver d;

	const std::string k = d.key("0001A34");

	BOOST_REQUIRE_EQUAL(k.size(), 6u);				// 2 + 4
	BOOST_CHECK_EQUAL((UCHAR) k[0], 0x00);			// big endian high byte
	BOOST_CHECK_EQUAL((UCHAR) k[1], 0x04);
	BOOST_CHECK_EQUAL(k.substr(2), std::string("1A34"));

	// The empty class carries a length of zero and nothing else.
	const std::string empty = d.key("000");
	BOOST_REQUIRE_EQUAL(empty.size(), 2u);
	BOOST_CHECK_EQUAL((UCHAR) empty[0], 0x00);
	BOOST_CHECK_EQUAL((UCHAR) empty[1], 0x00);

	// Lowercase folds into the key, so keys of the same class are identical.
	BOOST_CHECK(d.key("a") == d.key("00A"));
}


BOOST_AUTO_TEST_CASE(KeyLengthAccountsForThePrefix)
{
	Driver d;

	BOOST_CHECK_EQUAL(d.keyLength(0), 2);
	BOOST_CHECK_EQUAL(d.keyLength(20), 22);
	BOOST_CHECK_EQUAL(d.keyLength(32000), 32002);
}


// A partial key cannot be expressed in this format, and the contract is to
// return an empty key, never INTL_BAD_KEY_LENGTH.
BOOST_AUTO_TEST_CASE(PartialKeyIsEmpty)
{
	Driver d;

	for (const std::string& v : allValues())
	{
		BOOST_TEST_CONTEXT("'" << v << "'")
		{
			BOOST_CHECK_EQUAL(d.key(v, INTL_KEY_PARTIAL).size(), 0u);
		}
	}
}


BOOST_AUTO_TEST_CASE(UniqueKeyEqualsSortKey)
{
	Driver d;

	for (const std::string& v : allValues())
	{
		BOOST_TEST_CONTEXT("'" << v << "'")
		{
			BOOST_CHECK(d.key(v, INTL_KEY_UNIQUE) == d.key(v, INTL_KEY_SORT));
		}
	}
}


// The buffer math at full field width: a VARCHAR(20) of non strippable
// characters must fit in exactly key_length(20) bytes, with no overflow and
// no INTL_BAD_KEY_LENGTH.
BOOST_AUTO_TEST_CASE(FullWidthValueFitsInTheDeclaredKeyLength)
{
	Driver d;

	const std::string wide(20, '9');
	const USHORT declared = d.keyLength(20);

	UCHAR buffer[64];
	memset(buffer, 0xCC, sizeof(buffer));

	const USHORT len = d.tt.texttype_fn_string_to_key(&d.tt,
		(USHORT) wide.size(), (const UCHAR*) wide.data(),
		declared, buffer, INTL_KEY_SORT);

	BOOST_REQUIRE(len != INTL_BAD_KEY_LENGTH);
	BOOST_CHECK_EQUAL(len, declared);
	BOOST_CHECK_EQUAL((UCHAR) buffer[0], 0x00);
	BOOST_CHECK_EQUAL((UCHAR) buffer[1], 20);
	BOOST_CHECK_EQUAL(memcmp(buffer + 2, wide.data(), 20), 0);

	// Nothing was written past the declared length.
	BOOST_CHECK_EQUAL((UCHAR) buffer[declared], 0xCC);
}


// A destination smaller than len + 2 is a real case, not a bug: INTL_key_length
// caps the key at MAX_KEY and then raises it back to the raw field length
// (intl.cpp:1013-1017), so a wide column asks for more than it gets. The body
// is cut, the length prefix is not, and nothing is written past dstLen.
BOOST_AUTO_TEST_CASE(ShortDestinationTruncatesTheBodyOnly)
{
	Driver d;

	const std::string wide(20, '7');

	UCHAR buffer[16];
	memset(buffer, 0xCC, sizeof(buffer));

	const USHORT len = d.tt.texttype_fn_string_to_key(&d.tt,
		(USHORT) wide.size(), (const UCHAR*) wide.data(),
		8, buffer, INTL_KEY_SORT);

	BOOST_REQUIRE(len != INTL_BAD_KEY_LENGTH);
	BOOST_CHECK_EQUAL(len, 8);
	BOOST_CHECK_EQUAL((UCHAR) buffer[0], 0x00);
	BOOST_CHECK_EQUAL((UCHAR) buffer[1], 20);		// the true length, not 6
	BOOST_CHECK_EQUAL(memcmp(buffer + 2, wide.data(), 6), 0);
	BOOST_CHECK_EQUAL((UCHAR) buffer[8], 0xCC);
}


// PAD SPACE decides whether trailing spaces count towards the length, and the
// length is now what drives the order, so the two attribute sets must not be
// silently interchangeable.
BOOST_AUTO_TEST_CASE(PadOptionChangesTheNormalizedLength)
{
	Driver padded(TEXTTYPE_ATTR_PAD_SPACE | TEXTTYPE_ATTR_CASE_INSENSITIVE);
	Driver unpadded(TEXTTYPE_ATTR_CASE_INSENSITIVE);

	BOOST_CHECK_EQUAL(padded.compare("A", "A  "), 0);
	BOOST_CHECK_EQUAL(padded.key("A").size(), 3u);
	BOOST_CHECK_EQUAL(padded.key("A  ").size(), 3u);

	// Without PAD SPACE the trailing spaces are part of the value, so 'A  ' is
	// three characters long and sorts after every one character value.
	BOOST_CHECK_EQUAL(unpadded.compare("A", "A  "), -1);
	BOOST_CHECK_EQUAL(unpadded.key("A  ").size(), 5u);
}


// The case that motivated the change: a CNPJ root range must not swallow an
// 11 digit CPF just because it is shorter.
BOOST_AUTO_TEST_CASE(RangeOverDifferentLengthsExcludesShorterValues)
{
	Driver d;

	const std::string low = "12345678000000";
	const std::string high = "12345678999999";

	BOOST_CHECK_EQUAL(d.compare("12345678000199", low), 1);
	BOOST_CHECK_EQUAL(d.compare("12345678000199", high), -1);

	// 11 digits: shorter, therefore below the lower bound, therefore out.
	BOOST_CHECK_EQUAL(d.compare("12345678901", low), -1);
}


BOOST_AUTO_TEST_SUITE_END()	// IdZpadKeySuite
BOOST_AUTO_TEST_SUITE_END()	// IntlSuite

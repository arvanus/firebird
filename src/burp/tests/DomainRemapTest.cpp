#include "firebird.h"
#include "boost/test/unit_test.hpp"
#include "../burp/domain_remap.h"
#include "firebird/impl/blr.h"

using namespace Firebird;
using namespace Burp;

// BOOST_TEST decomposes the expression and needs printable operands.
// Firebird::string has no ostream operator, so wrap comparisons in an extra
// pair of parentheses to keep Boost from decomposing them.

BOOST_AUTO_TEST_SUITE(BurpSuite)
BOOST_AUTO_TEST_SUITE(DomainRemapSuite)


BOOST_AUTO_TEST_CASE(ParseInlineRule)
{
	DomainRemap remap;
	remap.parse("TDR_CNPJ = VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE ISO8859_1_LTRIM_ZERO_AI");

	BOOST_TEST(remap.ruleCount() == 1u);
	BOOST_TEST((remap.rule(0).domainName == "TDR_CNPJ"));
	BOOST_TEST(remap.rule(0).blrType == blr_varying);
	BOOST_TEST(remap.rule(0).charLength == 20u);
	BOOST_TEST((remap.rule(0).charsetName == "ISO8859_1"));
	BOOST_TEST((remap.rule(0).collationName == "ISO8859_1_LTRIM_ZERO_AI"));
}

BOOST_AUTO_TEST_CASE(ParseCharAndOptionalClauses)
{
	DomainRemap remap;
	remap.parse("A = CHAR(5); B = VARCHAR(10) COLLATE PT_BR");

	BOOST_TEST(remap.ruleCount() == 2u);
	BOOST_TEST(remap.rule(0).blrType == blr_text);
	BOOST_TEST(remap.rule(0).charLength == 5u);
	BOOST_TEST(remap.rule(0).charsetName.isEmpty());
	BOOST_TEST(remap.rule(0).collationName.isEmpty());
	BOOST_TEST(remap.rule(1).charsetName.isEmpty());
	BOOST_TEST((remap.rule(1).collationName == "PT_BR"));
}

BOOST_AUTO_TEST_CASE(IgnoresCommentsAndBlankLines)
{
	DomainRemap remap;
	remap.parse("# comment\n\nA = CHAR(5)\n# another comment\n");

	BOOST_TEST(remap.ruleCount() == 1u);
	BOOST_TEST((remap.rule(0).domainName == "A"));
}

BOOST_AUTO_TEST_CASE(RejectsUnsupportedType)
{
	DomainRemap remap;
	BOOST_CHECK_THROW(remap.parse("A = INTEGER"), DomainRemapError);
}

BOOST_AUTO_TEST_CASE(RejectsMalformedRule)
{
	DomainRemap remap;
	BOOST_CHECK_THROW(remap.parse("A VARCHAR(10)"), DomainRemapError);
	BOOST_CHECK_THROW(remap.parse("A = VARCHAR()"), DomainRemapError);
	BOOST_CHECK_THROW(remap.parse("A = VARCHAR(0)"), DomainRemapError);
}

BOOST_AUTO_TEST_CASE(RejectsDuplicateDomain)
{
	DomainRemap remap;
	BOOST_CHECK_THROW(remap.parse("A = CHAR(5); A = CHAR(6)"), DomainRemapError);
}

BOOST_AUTO_TEST_CASE(FindRuleHandlesSpacePaddedName)
{
	DomainRemap remap;
	remap.parse("TDR_CNPJ = VARCHAR(20)");

	// RDB$FIELD_NAME arrives as CHAR(63), space padded
	char padded[64];
	memset(padded, ' ', sizeof(padded));
	memcpy(padded, "TDR_CNPJ", 8);

	BOOST_TEST(remap.findRule(padded, sizeof(padded)) != nullptr);

	char other[64];
	memset(other, ' ', sizeof(other));
	memcpy(other, "TDR_CNPJ2", 9);

	BOOST_TEST(remap.findRule(other, sizeof(other)) == nullptr);
}

BOOST_AUTO_TEST_CASE(DomainNameIsCaseInsensitive)
{
	DomainRemap remap;
	remap.parse("tdr_cnpj = VARCHAR(20)");
	BOOST_TEST((remap.rule(0).domainName == "TDR_CNPJ"));
}

BOOST_AUTO_TEST_CASE(ParseUncheckedFlag)
{
	DomainRemap remap;
	remap.parse("A = VARCHAR(17) COLLATE PT_BR UNCHECKED; B = VARCHAR(20)");

	BOOST_TEST(remap.rule(0).uncheckedWidth == true);
	BOOST_TEST((remap.rule(0).collationName == "PT_BR"));
	BOOST_TEST(remap.rule(1).uncheckedWidth == false);
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

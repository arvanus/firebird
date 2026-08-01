#include "firebird.h"
#include "boost/test/unit_test.hpp"
#include "../burp/domain_remap.h"
#include "../common/classes/TempFile.h"
#include "firebird/impl/blr.h"
#include <stdio.h>

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

BOOST_AUTO_TEST_CASE(FindRuleHandlesNullTerminatedNameWithGarbageTail)
{
	DomainRemap remap;
	remap.parse("TDR_CNPJ = VARCHAR(20)");

	// GPRE declares RDB$FIELD_NAME as a null terminated string inside a much
	// larger buffer and never clears the tail, so everything past the
	// terminator is whatever happened to be on the stack. Fill the tail with a
	// non blank byte to stand in for that.
	char buffer[253];
	memset(buffer, 0xCC, sizeof(buffer));
	memcpy(buffer, "TDR_CNPJ", 8);
	buffer[8] = '\0';

	BOOST_TEST(remap.findRule(buffer, sizeof(buffer)) != nullptr);

	memset(buffer, 0xCC, sizeof(buffer));
	memcpy(buffer, "TDR_CNPJ2", 9);
	buffer[9] = '\0';

	BOOST_TEST(remap.findRule(buffer, sizeof(buffer)) == nullptr);
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


BOOST_AUTO_TEST_CASE(LoadFileHandlesLineLongerThanBuffer)
{
	// loadFile() reads in 1024 byte chunks. Pad the collation name well past
	// three chunk boundaries, so a naive per-chunk reassembly would inject
	// stray newlines in the middle of the rule and break it in two.
	const size_t padLength = 2600;
	string padding;

	for (size_t i = 0; i < padLength; ++i)
		padding += 'X';

	const PathName path = TempFile::create("fbtest");
	BOOST_REQUIRE(path.hasData());

	FILE* const file = fopen(path.c_str(), "wt");
	BOOST_REQUIRE(file != nullptr);

	fprintf(file, "LONGRULE = VARCHAR(20) COLLATE %s\nSHORT = CHAR(5)\n", padding.c_str());
	fclose(file);

	string spec;
	spec.printf("@%s", path.c_str());

	DomainRemap remap;
	remap.parse(spec.c_str());

	remove(path.c_str());

	BOOST_TEST(remap.ruleCount() == 2u);
	BOOST_TEST((remap.rule(0).domainName == "LONGRULE"));
	BOOST_TEST(remap.rule(0).collationName.length() == padLength);
	BOOST_TEST((remap.rule(0).collationName == padding));
	BOOST_TEST((remap.rule(1).domainName == "SHORT"));
}

BOOST_AUTO_TEST_CASE(ParseReplacesRulesFromPreviousCall)
{
	DomainRemap remap;
	remap.parse("A = CHAR(5); B = CHAR(6)");
	BOOST_TEST(remap.ruleCount() == 2u);

	remap.parse("C = CHAR(7)");

	BOOST_TEST(remap.ruleCount() == 1u);
	BOOST_TEST((remap.rule(0).domainName == "C"));
}

BOOST_AUTO_TEST_CASE(ParseClearsPartialRulesAfterThrow)
{
	DomainRemap remap;
	BOOST_CHECK_THROW(remap.parse("A = CHAR(5); B = INTEGER"), DomainRemapError);

	remap.parse("C = CHAR(7)");

	BOOST_TEST(remap.ruleCount() == 1u);
	BOOST_TEST((remap.rule(0).domainName == "C"));
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()

/*
 *	PROGRAM:	JRD Backup and Restore Program
 *	MODULE:		domain_remap.cpp
 *	DESCRIPTION:	Domain redefinition rules for restore (-FIX_DOMAINS)
 */

#include "firebird.h"
#include "../burp/domain_remap.h"
#include "../common/os/os_utils.h"
#include "firebird/impl/blr.h"
#include <stdio.h>

using namespace Firebird;

namespace {

	// Trims blanks and tabs from both ends.
	void trim(string& s)
	{
		const char* const blanks = " \t\r\n";
		const string::size_type first = s.find_first_not_of(blanks);

		if (first == string::npos)
		{
			s = "";
			return;
		}

		const string::size_type last = s.find_last_not_of(blanks);
		s = s.substr(first, last - first + 1);
	}

	// Splits "VARCHAR(20)" into type keyword and length.
	// Returns false if the shape is not <word>(<digits>).
	bool splitTypeSpec(const string& token, string& keyword, USHORT& length)
	{
		const string::size_type open = token.find('(');

		if (open == string::npos || token[token.length() - 1] != ')')
			return false;

		keyword = token.substr(0, open);
		const string digits = token.substr(open + 1, token.length() - open - 2);

		if (digits.isEmpty())
			return false;

		for (string::size_type i = 0; i < digits.length(); ++i)
		{
			if (digits[i] < '0' || digits[i] > '9')
				return false;
		}

		const int value = atoi(digits.c_str());

		if (value <= 0 || value > MAX_SSHORT)
			return false;

		length = (USHORT) value;
		return true;
	}

} // anonymous namespace

namespace Burp {

void DomainRemap::parse(const char* spec)
{
	// Each call replaces any rules from a previous call, and leaves nothing
	// behind if this call itself throws partway through.
	m_rules.clear();

	string text;

	if (spec && spec[0] == '@')
		loadFile(spec + 1, text);
	else
		text = spec ? spec : "";

	parseRules(text);

	if (m_rules.isEmpty())
		throw DomainRemapError("no domain remap rule was given");
}

void DomainRemap::loadFile(const char* path, string& text)
{
	FILE* const file = os_utils::fopen(path, "rt");

	if (!file)
	{
		string msg;
		msg.printf("cannot open domain remap file %s", path);
		throw DomainRemapError(msg);
	}

	char buffer[1024];

	while (fgets(buffer, sizeof(buffer), file))
		text += buffer;

	fclose(file);

	// fgets keeps the newline it read, if any. A line longer than the buffer
	// is delivered as several chunks with no newline in between, so nothing
	// may be inserted between chunks - that would cut one rule into two
	// malformed fragments. Only a file whose last line has no trailing
	// newline needs one added, so the last rule is properly terminated.
	if (text.hasData() && text[text.length() - 1] != '\n')
		text += '\n';
}

void DomainRemap::parseRules(const string& text)
{
	string pending;

	for (string::size_type i = 0; i <= text.length(); ++i)
	{
		const char c = (i < text.length()) ? text[i] : ';';

		if (c == ';' || c == '\n')
		{
			trim(pending);

			if (pending.hasData() && pending[0] != '#')
				parseOneRule(pending);

			pending = "";
		}
		else if (c == '#')
		{
			// comment runs to end of line
			while (i < text.length() && text[i] != '\n')
				++i;

			trim(pending);

			if (pending.hasData())
				parseOneRule(pending);

			pending = "";
		}
		else
			pending += c;
	}
}

void DomainRemap::parseOneRule(const string& line)
{
	const string::size_type equals = line.find('=');

	if (equals == string::npos)
	{
		string msg;
		msg.printf("malformed domain remap rule: %s", line.c_str());
		throw DomainRemapError(msg);
	}

	RemapRule rule(*getDefaultMemoryPool());

	rule.domainName = line.substr(0, equals);
	trim(rule.domainName);
	rule.domainName.upper();

	if (rule.domainName.isEmpty())
	{
		string msg;
		msg.printf("missing domain name in rule: %s", line.c_str());
		throw DomainRemapError(msg);
	}

	for (unsigned i = 0; i < m_rules.getCount(); ++i)
	{
		if (m_rules[i].domainName == rule.domainName)
		{
			string msg;
			msg.printf("domain %s appears in more than one rule", rule.domainName.c_str());
			throw DomainRemapError(msg);
		}
	}

	string rest = line.substr(equals + 1);
	trim(rest);
	rest.upper();

	// Tokenize on blanks.
	ObjectsArray<string> tokens;
	string current;

	for (string::size_type i = 0; i <= rest.length(); ++i)
	{
		const char c = (i < rest.length()) ? rest[i] : ' ';

		if (c == ' ' || c == '\t')
		{
			if (current.hasData())
			{
				tokens.add(current);
				current = "";
			}
		}
		else
			current += c;
	}

	if (tokens.isEmpty())
	{
		string msg;
		msg.printf("missing target type in rule for domain %s", rule.domainName.c_str());
		throw DomainRemapError(msg);
	}

	string keyword;

	if (!splitTypeSpec(tokens[0], keyword, rule.charLength))
	{
		string msg;
		msg.printf("invalid target type %s for domain %s",
			tokens[0].c_str(), rule.domainName.c_str());
		throw DomainRemapError(msg);
	}

	if (keyword == "VARCHAR")
		rule.blrType = blr_varying;
	else if (keyword == "CHAR")
		rule.blrType = blr_text;
	else
	{
		string msg;
		msg.printf("unsupported target type %s for domain %s, use CHAR or VARCHAR",
			keyword.c_str(), rule.domainName.c_str());
		throw DomainRemapError(msg);
	}

	// Optional clauses: CHARACTER SET <name>, COLLATE <name>
	unsigned pos = 1;

	while (pos < tokens.getCount())
	{
		if (tokens[pos] == "CHARACTER" && pos + 2 < tokens.getCount() && tokens[pos + 1] == "SET")
		{
			rule.charsetName = tokens[pos + 2];
			pos += 3;
		}
		else if (tokens[pos] == "COLLATE" && pos + 1 < tokens.getCount())
		{
			rule.collationName = tokens[pos + 1];
			pos += 2;
		}
		else if (tokens[pos] == "UNCHECKED")
		{
			rule.uncheckedWidth = true;
			++pos;
		}
		else
		{
			string msg;
			msg.printf("unexpected token %s in rule for domain %s",
				tokens[pos].c_str(), rule.domainName.c_str());
			throw DomainRemapError(msg);
		}
	}

	m_rules.add(rule);
}

RemapRule* DomainRemap::findRule(const char* name, size_t nameSize)
{
	// The name may arrive blank padded, or as a null terminated string sitting
	// in a much larger buffer whose tail was never initialized: GPRE declares
	// RDB$FIELD_NAME that way. Stop at the first NUL so the garbage after it is
	// never compared, then drop any trailing blanks.
	size_t length = 0;

	while (length < nameSize && name[length] != '\0')
		++length;

	while (length > 0 && name[length - 1] == ' ')
		--length;

	for (unsigned i = 0; i < m_rules.getCount(); ++i)
	{
		RemapRule& rule = m_rules[i];

		if (rule.domainName.length() == length &&
			memcmp(rule.domainName.c_str(), name, length) == 0)
		{
			return &rule;
		}
	}

	return nullptr;
}

} // namespace Burp

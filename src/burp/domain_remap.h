/*
 *	PROGRAM:	JRD Backup and Restore Program
 *	MODULE:		domain_remap.h
 *	DESCRIPTION:	Domain redefinition rules for restore (-FIX_DOMAINS)
 */

#ifndef BURP_DOMAIN_REMAP_H
#define BURP_DOMAIN_REMAP_H

#include "../common/classes/fb_string.h"
#include "../common/classes/objects_array.h"

namespace Burp {

// Raised for any problem in the rules themselves. Caught by the caller,
// which turns it into a BURP_error with the proper catalog message.
class DomainRemapError
{
public:
	explicit DomainRemapError(const Firebird::string& text)
		: message(text)
	{
	}

	Firebird::string message;
};

struct RemapRule
{
	// ObjectsArray<T>::add builds elements as T(pool, item), so both of these
	// are mandatory: objects_array.h:211 will not compile without them.
	explicit RemapRule(Firebird::MemoryPool& pool)
		: domainName(pool), blrType(0), charLength(0),
		  charsetName(pool), collationName(pool),
		  uncheckedWidth(false), applied(false)
	{
	}

	RemapRule(Firebird::MemoryPool& pool, const RemapRule& other)
		: domainName(pool, other.domainName), blrType(other.blrType),
		  charLength(other.charLength),
		  charsetName(pool, other.charsetName),
		  collationName(pool, other.collationName),
		  uncheckedWidth(other.uncheckedWidth), applied(other.applied)
	{
	}

	Firebird::string	domainName;		// uppercase, trimmed
	UCHAR				blrType;		// blr_text or blr_varying
	USHORT				charLength;		// characters, not bytes
	Firebird::string	charsetName;	// empty means database default
	Firebird::string	collationName;	// empty means charset default
	bool				uncheckedWidth;	// UNCHECKED: skip the DDL minimum width rule
	bool				applied;		// set when get_global_field matched it
};

class DomainRemap
{
public:
	DomainRemap()
		: m_rules(*getDefaultMemoryPool())
	{
	}

	explicit DomainRemap(Firebird::MemoryPool& pool)
		: m_rules(pool)
	{
	}

	// spec is either "@path/to/file" or the rules themselves, separated by ';'
	void parse(const char* spec);

	// name is RDB$FIELD_NAME, space padded to nameSize
	RemapRule* findRule(const char* name, size_t nameSize);

	unsigned ruleCount() const
	{
		return (unsigned) m_rules.getCount();
	}

	const RemapRule& rule(unsigned index) const
	{
		return m_rules[index];
	}

	RemapRule& rule(unsigned index)
	{
		return m_rules[index];
	}

private:
	void parseRules(const Firebird::string& text);
	void parseOneRule(const Firebird::string& line);
	void loadFile(const char* path, Firebird::string& text);

	Firebird::ObjectsArray<RemapRule> m_rules;
};

} // namespace Burp

#endif // BURP_DOMAIN_REMAP_H

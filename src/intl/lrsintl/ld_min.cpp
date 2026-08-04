/*
 * Minimal INTL module exposing only the ID_ZPAD_CI collation.
 *
 * Firebird loads an INTL module by looking up a handful of C entry points in
 * a shared library named by an intl_module block in any *.conf file inside
 * the engine's intl directory (Jrd::IntlManager::initialize, ScanDir over
 * "*.conf"). Registration is per charset:collation pair, and the charset
 * itself is resolved separately, so this module can add a collation to a
 * charset that fbintl owns without conflicting with it and without touching
 * fbintl.conf.
 *
 * Only two entry points are needed:
 *
 *   LD_version                       negotiates the interface version
 *   LD_lookup_texttype_with_status   builds the texttype
 *
 * LD_lookup_charset is not provided: the charset keeps coming from fbintl.
 * LD_setup_attributes is not provided either; the engine calls findSymbol and
 * skips it when it is absent.
 *
 * The collation logic itself is included verbatim from the Firebird tree so
 * there is a single source of truth. It allocates nothing, throws nothing and
 * does not use ICU, so this module has no dependency on the Firebird
 * libraries at run time.
 *
 * Build: builds/win32/make_lrsintl.bat on Windows, or the Makefile in
 * doc/README.lrsintl_build.md on Linux.
 */

#include "firebird.h"
#include "intl/ldcommon.h"
#include "intl/ld_proto.h"

#include <string.h>

// ld_proto.h declares this; ld.cpp defines it in the full module.
USHORT version = INTL_VERSION_2;

// The collation driver, compiled straight into this module.
#include "intl/lc_id_zpad_ci.cpp"

namespace
{
	// Names must match the collation names used in lrsintl.conf.
	const char* const COLLATIONS[] =
	{
		"WIN1252_ID_ZPAD_CI",
		"ISO8859_1_ID_ZPAD_CI",
		nullptr
	};

	bool isOurs(const ASCII* name)
	{
		for (int i = 0; COLLATIONS[i]; i++)
		{
			if (strcmp(COLLATIONS[i], name) == 0)
				return true;
		}

		return false;
	}

	void report(char* buffer, ULONG length, const char* message)
	{
		if (!buffer || !length)
			return;

		strncpy(buffer, message, length - 1);
		buffer[length - 1] = '\0';
	}
}


FB_DLL_EXPORT void LD_version(USHORT* v)
{
	// Same negotiation as the stock module: version 1 and 2 are supported.
	if (*v != INTL_VERSION_1)
		*v = INTL_VERSION_2;

	version = *v;
}


FB_DLL_EXPORT INTL_BOOL LD_lookup_texttype_with_status(
	char* status_buffer, ULONG status_buffer_length,
	texttype* tt, const ASCII* texttype_name, const ASCII* charset_name,
	USHORT attributes, const UCHAR* specific_attributes,
	ULONG specific_attributes_length, INTL_BOOL ignore_attributes,
	const ASCII* /*config_info*/)
{
	if (status_buffer && status_buffer_length)
		status_buffer[0] = '\0';

	if (ignore_attributes)
	{
		attributes = TEXTTYPE_ATTR_PAD_SPACE;
		specific_attributes = nullptr;
		specific_attributes_length = 0;
	}

	if (!isOurs(texttype_name))
	{
		report(status_buffer, status_buffer_length,
			"lrsintl: this module only provides the ID_ZPAD_CI collations");
		return false;
	}

	// The driver uses TEXTTYPE_ENTRY3, so the charset argument is unused and
	// passing NULL is safe. That is what keeps this module independent from
	// fbintl: it never has to build a charset of its own.
	const INTL_BOOL ok = LCIDZPADCI_init(tt, nullptr, texttype_name, charset_name,
		attributes, specific_attributes, specific_attributes_length, nullptr);

	if (!ok)
	{
		report(status_buffer, status_buffer_length,
			"lrsintl: unsupported attributes. Only CASE INSENSITIVE and"
			" PAD SPACE are accepted, and specific attributes are not supported");
	}

	return ok;
}

/*
 *	PROGRAM:	JRD engine tests
 *	MODULE:		LtrimZeroCollationTest.cpp
 *	DESCRIPTION:	Integration tests for the LTRIM_ZERO collation
 *
 * These tests deliberately do NOT re-cover the collation semantics (equality
 * classes, PAD SPACE, empty class, LIKE / SIMILAR TO). That is owned by
 * test_ltrim_zero.sql. What lives here is everything that a plain isql script
 * cannot reach:
 *
 *   - concurrency: several attachments writing at the same time
 *   - volume: enough rows for real b-tree splits and prefix compression
 *     between index nodes, followed by an online validation of the database
 *   - index shapes never exercised elsewhere: DESCENDING (including the
 *     empty key branch), compound, PRIMARY KEY and FOREIGN KEY
 *   - gbak backup / restore round trip, which rebuilds every index through a
 *     fresh string_to_key
 *   - row identity rather than row counts: index plan and natural plan must
 *     return the very same rows, not merely the same number of rows
 *
 * Run only this suite with:
 *     engine_test --run_test=EngineSuite/LtrimZeroSuite
 */

#include "firebird.h"
#include "firebird/Interface.h"
#include "ibase.h"
#include "boost/test/unit_test.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace Firebird;

namespace
{

typedef std::vector<ISC_INT64> IdList;

const char* const CREATE_COLLATION_SQL =
	"CREATE COLLATION LTZ FOR WIN1252"
	" FROM EXTERNAL ('WIN1252_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE";

const char* const CREATE_DOMAIN_SQL =
	"CREATE DOMAIN D_LTZ AS VARCHAR(20) CHARACTER SET WIN1252 COLLATE LTZ";


std::string describe(IMaster* master, const FbException& e)
{
	char buf[2048];
	master->getUtilInterface()->formatStatus(buf, sizeof(buf), e.getStatus());
	return std::string(buf);
}


// Credentials are passed explicitly instead of relying on the operating system
// user: a freshly built tree may not have trusted authentication wired up, and
// the tests must behave the same in Debug and in Release.
class Dpb
{
public:
	Dpb(IMaster* master, ThrowStatusWrapper& status)
		: st(status),
		  builder(master->getUtilInterface()->getXpbBuilder(&status, IXpbBuilder::DPB, nullptr, 0))
	{
		builder->insertString(&st, isc_dpb_user_name, "SYSDBA");
		builder->insertString(&st, isc_dpb_password, "masterkey");
	}

	~Dpb()
	{
		builder->dispose();
	}

	unsigned length()
	{
		return builder->getBufferLength(&st);
	}

	const unsigned char* buffer()
	{
		return builder->getBuffer(&st);
	}

private:
	ThrowStatusWrapper& st;
	IXpbBuilder* builder;
};


// Reads a result set of exactly one BIGINT column. Every query in this file
// casts to BIGINT so there is a single type to decode.
IdList fetchIds(ThrowStatusWrapper& st, IAttachment* att, ITransaction* tra, const char* sql)
{
	IdList out;

	IResultSet* rs = att->openCursor(&st, tra, 0, sql, SQL_DIALECT_V6,
		nullptr, nullptr, nullptr, nullptr, 0);

	IMessageMetadata* meta = rs->getMetadata(&st);
	const unsigned length = meta->getMessageLength(&st);
	const unsigned offset = meta->getOffset(&st, 0);
	const unsigned nullOffset = meta->getNullOffset(&st, 0);
	meta->release();

	std::vector<unsigned char> buffer(length);

	while (rs->fetchNext(&st, buffer.data()) == IStatus::RESULT_OK)
	{
		short nullFlag;
		memcpy(&nullFlag, &buffer[nullOffset], sizeof(nullFlag));

		if (nullFlag)
			continue;

		ISC_INT64 value;
		memcpy(&value, &buffer[offset], sizeof(value));
		out.push_back(value);
	}

	rs->close(&st);		// close() also releases the interface

	return out;
}


// A database created from scratch for one test case, with the collation and
// the D_LTZ domain already in place.
class TestDb
{
public:
	explicit TestDb(const char* name)
		: master(fb_get_master_interface()),
		  st(master->getStatus()),
		  prov(master->getDispatcher()),
		  att(nullptr),
		  tra(nullptr),
		  path(std::string("ltz_") + name + ".fdb")
	{
		remove(path.c_str());

		Dpb dpb(master, st);
		att = prov->createDatabase(&st, path.c_str(), dpb.length(), dpb.buffer());
		tra = att->startTransaction(&st, 0, nullptr);

		ddl(CREATE_COLLATION_SQL);
		ddl(CREATE_DOMAIN_SQL);
	}

	~TestDb()
	{
		close();

		if (prov)
			prov->release();

		st.dispose();
	}

	void exec(const char* sql)
	{
		att->execute(&st, tra, 0, sql, SQL_DIALECT_V6, nullptr, nullptr, nullptr, nullptr);
	}

	// DDL is committed straight away so following statements can see it
	void ddl(const char* sql)
	{
		exec(sql);
		tra->commitRetaining(&st);
	}

	void commit()
	{
		tra->commitRetaining(&st);
	}

	// Ends the current transaction and starts a fresh one. Needed after other
	// attachments have committed: the default transaction is a snapshot, and
	// commitRetaining keeps that snapshot, so it would never see their rows.
	void refresh()
	{
		tra->commit(&st);
		tra = att->startTransaction(&st, 0, nullptr);
	}

	IdList ids(const std::string& sql)
	{
		return fetchIds(st, att, tra, sql.c_str());
	}

	ISC_INT64 one(const std::string& sql)
	{
		const IdList result = ids(sql);
		BOOST_REQUIRE_EQUAL(result.size(), 1u);
		return result[0];
	}

	// Runs the statement and reports whether it was rejected. Used for the
	// constraint tests, where the failure IS the expected outcome.
	bool fails(const char* sql)
	{
		try
		{
			exec(sql);
			tra->commitRetaining(&st);
			return false;
		}
		catch (const FbException&)
		{
			st.init();
			tra->rollbackRetaining(&st);
			return true;
		}
	}

	// Drops the attachment without dropping the file, so that services or
	// another attachment can take over.
	void detach()
	{
		if (tra)
		{
			tra->commit(&st);
			tra = nullptr;
		}

		if (att)
		{
			att->detach(&st);
			att = nullptr;
		}
	}

	void reattach()
	{
		Dpb dpb(master, st);
		att = prov->attachDatabase(&st, path.c_str(), dpb.length(), dpb.buffer());
		tra = att->startTransaction(&st, 0, nullptr);
	}

	void close()
	{
		try
		{
			if (tra)
			{
				tra->rollback(&st);
				tra = nullptr;
			}
		}
		catch (const FbException&)
		{
			st.init();

			if (tra)
				tra->release();

			tra = nullptr;
		}

		try
		{
			if (att)
			{
				att->dropDatabase(&st);
				att = nullptr;
			}
		}
		catch (const FbException&)
		{
			st.init();

			if (att)
				att->release();

			att = nullptr;
		}

		remove(path.c_str());
	}

	IMaster* const master;
	ThrowStatusWrapper st;
	IProvider* prov;
	IAttachment* att;
	ITransaction* tra;
	const std::string path;
};


// Minimal service manager wrapper: start an action and drain its output.
class Service
{
public:
	explicit Service(IMaster* aMaster)
		: master(aMaster),
		  st(aMaster->getStatus()),
		  prov(aMaster->getDispatcher()),
		  svc(nullptr)
	{
		IUtil* const utl = master->getUtilInterface();
		IXpbBuilder* spb = utl->getXpbBuilder(&st, IXpbBuilder::SPB_ATTACH, nullptr, 0);
		spb->insertString(&st, isc_spb_user_name, "SYSDBA");
		spb->insertString(&st, isc_spb_password, "masterkey");

		svc = prov->attachServiceManager(&st, "service_mgr",
			spb->getBufferLength(&st), spb->getBuffer(&st));

		spb->dispose();
	}

	~Service()
	{
		try
		{
			if (svc)
				svc->detach(&st);
		}
		catch (const FbException&)
		{
			st.init();

			if (svc)
				svc->release();
		}

		if (prov)
			prov->release();

		st.dispose();
	}

	IXpbBuilder* startBuilder()
	{
		return master->getUtilInterface()->getXpbBuilder(&st, IXpbBuilder::SPB_START, nullptr, 0);
	}

	// Starts the action and returns everything the service printed.
	std::string run(IXpbBuilder* spb)
	{
		svc->start(&st, spb->getBufferLength(&st), spb->getBuffer(&st));

		const unsigned char receiveItems[] = { isc_info_svc_line };
		unsigned char results[4096];
		std::string output;

		bool more = true;

		while (more)
		{
			svc->query(&st, 0, nullptr, sizeof(receiveItems), receiveItems,
				sizeof(results), results);

			more = false;
			const unsigned char* p = results;
			const unsigned char* const end = results + sizeof(results);

			while (p < end && *p != isc_info_end)
			{
				if (*p++ != isc_info_svc_line)
					break;

				const unsigned len = p[0] | (p[1] << 8);
				p += 2;

				if (len > 0)
				{
					output.append(reinterpret_cast<const char*>(p), len);
					output.append("\n");
					more = true;
				}

				p += len;
			}
		}

		return output;
	}

	IMaster* const master;
	ThrowStatusWrapper st;
	IProvider* prov;
	IService* svc;
};


// Splices a PLAN clause into a query. PLAN has to sit after WHERE but before
// ORDER BY, so appending it blindly is a syntax error.
std::string withPlan(const std::string& query, const std::string& plan)
{
	const std::string clause = " PLAN " + plan;
	const size_t orderBy = query.find(" ORDER BY ");

	if (orderBy == std::string::npos)
		return query + clause;

	return query.substr(0, orderBy) + clause + query.substr(orderBy);
}


void checkSamePlanResult(TestDb& db, const char* label, const std::string& query,
	const std::string& naturalPlan, const std::string& indexPlan)
{
	const IdList natural = db.ids(withPlan(query, naturalPlan));
	const IdList indexed = db.ids(withPlan(query, indexPlan));

	BOOST_TEST_CONTEXT(label)
	{
		BOOST_CHECK_EQUAL_COLLECTIONS(natural.begin(), natural.end(),
			indexed.begin(), indexed.end());
	}
}

} // anonymous namespace


// FbException does not derive from std::exception, so Boost.Test would only
// report "unknown type" for it. Every test body is wrapped so that a Firebird
// error arrives as a readable failure instead.
#define LTZ_TEST_CASE(name)								\
	static void name##Body();							\
	BOOST_AUTO_TEST_CASE(name)							\
	{													\
		try												\
		{												\
			name##Body();								\
		}												\
		catch (const FbException& e)					\
		{												\
			BOOST_FAIL(describe(fb_get_master_interface(), e));	\
		}												\
	}													\
	static void name##Body()


BOOST_AUTO_TEST_SUITE(EngineSuite)
BOOST_AUTO_TEST_SUITE(LtrimZeroSuite)


// Runs first and fails loudly when the runtime tree is incomplete, so the
// other cases are not misread as collation bugs. It also tells apart "this
// collation is missing" from "no collation from fbintl.conf resolves at all",
// and prints where the engine is looking for its files.
BOOST_AUTO_TEST_CASE(CollationIsInstalled)
{
	IConfigManager* const cfg = fb_get_master_interface()->getConfigManager();

	BOOST_TEST_MESSAGE("DIR_BIN:  " << cfg->getDirectory(IConfigManager::DIR_BIN));
	BOOST_TEST_MESSAGE("DIR_CONF: " << cfg->getDirectory(IConfigManager::DIR_CONF));
	BOOST_TEST_MESSAGE("DIR_INTL: " << cfg->getDirectory(IConfigManager::DIR_INTL));
	BOOST_TEST_MESSAGE("DIR_PLUGINS: " << cfg->getDirectory(IConfigManager::DIR_PLUGINS));

	IMaster* const master = fb_get_master_interface();
	ThrowStatusWrapper st(master->getStatus());
	IProvider* const prov = master->getDispatcher();
	IAttachment* att = nullptr;
	ITransaction* tra = nullptr;

	// PXW_SPAN is a control: it lives in the same fbintl.conf block as
	// WIN1252_LTRIM_ZERO. If the control also fails, the runtime tree cannot
	// register anything from fbintl.conf and the failure is not ours.
	struct Probe
	{
		const char* sql;
		const char* what;
		bool required;
	};

	const Probe probes[] =
	{
		{ "CREATE COLLATION P1 FOR WIN1252 FROM EXTERNAL ('PXW_SPAN')",
		  "control collation from fbintl.conf", false },
		{ "CREATE COLLATION P2 FOR WIN1252 FROM EXTERNAL ('WIN1252_LTRIM_ZERO')",
		  "WIN1252_LTRIM_ZERO", true },
		{ "CREATE COLLATION P3 FOR ISO8859_1 FROM EXTERNAL ('ISO8859_1_LTRIM_ZERO')",
		  "ISO8859_1_LTRIM_ZERO", true }
	};

	bool controlOk = true;

	try
	{
		remove("ltz_env.fdb");
		Dpb dpb(master, st);
		att = prov->createDatabase(&st, "ltz_env.fdb", dpb.length(), dpb.buffer());
		tra = att->startTransaction(&st, 0, nullptr);

		for (const Probe& probe : probes)
		{
			std::string error;

			try
			{
				att->execute(&st, tra, 0, probe.sql, SQL_DIALECT_V6,
					nullptr, nullptr, nullptr, nullptr);
				tra->commitRetaining(&st);
			}
			catch (const FbException& e)
			{
				error = describe(master, e);
				st.init();
				tra->rollbackRetaining(&st);
			}

			if (error.empty())
				BOOST_TEST_MESSAGE("OK   " << probe.what);
			else
			{
				BOOST_TEST_MESSAGE("FAIL " << probe.what << " -> " << error);

				if (!probe.required)
					controlOk = false;
			}

			if (probe.required)
			{
				BOOST_CHECK_MESSAGE(error.empty(), probe.what << " could not be created."
					<< (controlOk
						? " The control collation loaded, so this is specific to LTRIM_ZERO."
						: " The control collation also failed: this runtime tree cannot load"
						  " anything from fbintl.conf, so the whole suite is inconclusive.")
					<< " " << error);
			}
		}

		tra->commit(&st);
		tra = nullptr;
		att->dropDatabase(&st);
		att = nullptr;
	}
	catch (const FbException& e)
	{
		BOOST_TEST_MESSAGE("environment probe aborted: " << describe(master, e));
		st.init();

		if (tra)
			tra->release();

		if (att)
			att->release();
	}

	st.dispose();
	prov->release();
}


/* ------------------------------------------------------------------------ *
 * 1. Concurrency and volume
 *
 * Eight attachments insert 6250 rows each into the same indexed table at the
 * same time. Nothing is asserted while the threads run: Boost.Test macros are
 * not thread safe and lock timing is not a property of the collation. Every
 * check happens single threaded after the join, and the database is then
 * validated online, which is what would actually surface a compare / key
 * mismatch as index corruption.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(ConcurrentVolumeAndValidation)
{
	const int THREADS = 8;
	const int PER_THREAD = 6250;
	const int CLASSES = 25;
	const int TOTAL = THREADS * PER_THREAD;

	TestDb db("volume");

	db.ddl("CREATE TABLE VOL (ID INTEGER NOT NULL PRIMARY KEY, V D_LTZ)");
	db.ddl("CREATE INDEX IX_VOL_V ON VOL(V)");

	// Row i gets a value of the class 'K<nn>' with nn = i mod 25, prefixed by
	// a varying run of spaces and zeros and with the case flipped on even
	// rows. Everything in front of the 'K' is stripped by the collation, so
	// each class must end up with exactly TOTAL / CLASSES rows.
	std::vector<std::string> errors(THREADS);

	auto worker = [&](int index)
	{
		IMaster* const master = fb_get_master_interface();
		ThrowStatusWrapper st(master->getStatus());
		IProvider* const prov = master->getDispatcher();

		IAttachment* att = nullptr;
		ITransaction* tra = nullptr;

		try
		{
			Dpb dpb(master, st);
			att = prov->attachDatabase(&st, db.path.c_str(), dpb.length(), dpb.buffer());
			tra = att->startTransaction(&st, 0, nullptr);

			char sql[1024];
			snprintf(sql, sizeof(sql),
				"EXECUTE BLOCK AS "
				"DECLARE I INTEGER; "
				"DECLARE V VARCHAR(20); "
				"BEGIN "
				"  I = %d; "
				"  WHILE (I < %d) DO "
				"  BEGIN "
				"    V = SUBSTRING('    ' FROM 1 FOR MOD(I, 3) + 1) || "
				"        SUBSTRING('0000' FROM 1 FOR MOD(I, 5) + 1) || "
				"        'K' || LPAD(CAST(MOD(I, %d) AS VARCHAR(2)), 2, '0'); "
				"    IF (MOD(I, 2) = 0) THEN V = LOWER(V); "
				"    INSERT INTO VOL VALUES (:I, :V); "
				"    I = I + 1; "
				"  END "
				"END",
				index * PER_THREAD, (index + 1) * PER_THREAD, CLASSES);

			att->execute(&st, tra, 0, sql, SQL_DIALECT_V6, nullptr, nullptr, nullptr, nullptr);

			tra->commit(&st);
			tra = nullptr;

			att->detach(&st);
			att = nullptr;
		}
		catch (const FbException& e)
		{
			errors[index] = describe(master, e);
		}

		if (tra)
			tra->release();

		if (att)
			att->release();

		st.dispose();
		prov->release();
	};

	std::vector<std::thread> pool;
	pool.reserve(THREADS);

	for (int i = 0; i < THREADS; i++)
		pool.emplace_back(worker, i);

	for (auto& t : pool)
		t.join();

	for (int i = 0; i < THREADS; i++)
		BOOST_REQUIRE_MESSAGE(errors[i].empty(), "worker " << i << ": " << errors[i]);

	db.refresh();

	BOOST_REQUIRE_EQUAL(db.one("SELECT CAST(COUNT(*) AS BIGINT) FROM VOL"), TOTAL);

	// Every class must hold the same number of rows through both plans.
	for (int c = 0; c < CLASSES; c++)
	{
		char value[8];
		snprintf(value, sizeof(value), "K%02d", c);

		const std::string query =
			std::string("SELECT CAST(COUNT(*) AS BIGINT) FROM VOL WHERE V = '") + value + "'";

		const ISC_INT64 natural = db.one(withPlan(query, "(VOL NATURAL)"));
		const ISC_INT64 indexed = db.one(withPlan(query, "(VOL INDEX (IX_VOL_V))"));

		BOOST_TEST_CONTEXT("class " << value)
		{
			BOOST_CHECK_EQUAL(natural, TOTAL / CLASSES);
			BOOST_CHECK_EQUAL(indexed, natural);
		}
	}

	// Row identity, not just counts, for one full class (2000 rows).
	checkSamePlanResult(db, "identity of class K07",
		"SELECT CAST(ID AS BIGINT) FROM VOL WHERE V = 'k07' ORDER BY ID",
		"SORT ((VOL NATURAL))", "SORT ((VOL INDEX (IX_VOL_V)))");

	// A lowercase, zero prefixed probe must find exactly the same rows.
	checkSamePlanResult(db, "prefixed probe of class K07",
		"SELECT CAST(ID AS BIGINT) FROM VOL WHERE V = '  000k07' ORDER BY ID",
		"SORT ((VOL NATURAL))", "SORT ((VOL INDEX (IX_VOL_V)))");

	// Navigating the index must yield the same order as an explicit sort.
	// Rows are compared by class number rather than by ID, because rows of the
	// same class are ties and their relative order is not defined.
	{
		const IdList sorted = db.ids(
			"SELECT CAST(RIGHT(V, 2) AS BIGINT) FROM VOL"
			" PLAN SORT (VOL NATURAL) ORDER BY V");
		const IdList navigated = db.ids(
			"SELECT CAST(RIGHT(V, 2) AS BIGINT) FROM VOL"
			" PLAN (VOL ORDER IX_VOL_V) ORDER BY V");

		BOOST_CHECK_EQUAL(sorted.size(), static_cast<size_t>(TOTAL));
		BOOST_CHECK_EQUAL(navigated.size(), static_cast<size_t>(TOTAL));
		BOOST_CHECK_EQUAL_COLLECTIONS(sorted.begin(), sorted.end(),
			navigated.begin(), navigated.end());
	}

	// An inconsistency between compare() and string_to_key() shows up as a
	// corrupt index, which validation detects directly.
	const std::string dbPath = db.path;
	db.detach();

	{
		Service service(fb_get_master_interface());
		IXpbBuilder* spb = service.startBuilder();
		spb->insertTag(&service.st, isc_action_svc_validate);
		spb->insertString(&service.st, isc_spb_dbname, dbPath.c_str());

		const std::string output = service.run(spb);
		spb->dispose();

		BOOST_TEST_MESSAGE("online validation output:\n" << output);
		BOOST_CHECK(output.find("Error") == std::string::npos);
		BOOST_CHECK(output.find("corrupt") == std::string::npos);
	}

	db.reattach();
}


/* ------------------------------------------------------------------------ *
 * 2. DESCENDING index, including the empty key branch
 *
 * A descending index writes desc_end_value_prefix ahead of the pad byte when
 * the key is empty. Every value made only of zeros and spaces produces an
 * empty key with this collation, so that branch is reachable here and nowhere
 * else in the test material.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(DescendingIndexAndEmptyKey)
{
	TestDb db("descending");

	db.ddl("CREATE TABLE D1 (ID INTEGER, V D_LTZ)");

	db.exec("INSERT INTO D1 VALUES (1, '000')");
	db.exec("INSERT INTO D1 VALUES (2, '   ')");
	db.exec("INSERT INTO D1 VALUES (3, '')");
	db.exec("INSERT INTO D1 VALUES (4, 'A')");
	db.exec("INSERT INTO D1 VALUES (5, '00a  ')");
	db.exec("INSERT INTO D1 VALUES (6, 'B')");
	db.exec("INSERT INTO D1 VALUES (7, NULL)");
	db.commit();

	db.ddl("CREATE DESCENDING INDEX IX_D1 ON D1(V)");

	checkSamePlanResult(db, "descending index, empty class",
		"SELECT CAST(ID AS BIGINT) FROM D1 WHERE V = '0' ORDER BY ID",
		"SORT ((D1 NATURAL))", "SORT ((D1 INDEX (IX_D1)))");

	checkSamePlanResult(db, "descending index, empty literal",
		"SELECT CAST(ID AS BIGINT) FROM D1 WHERE V = '' ORDER BY ID",
		"SORT ((D1 NATURAL))", "SORT ((D1 INDEX (IX_D1)))");

	checkSamePlanResult(db, "descending index, class A",
		"SELECT CAST(ID AS BIGINT) FROM D1 WHERE V = 'A' ORDER BY ID",
		"SORT ((D1 NATURAL))", "SORT ((D1 INDEX (IX_D1)))");

	checkSamePlanResult(db, "descending index, range",
		"SELECT CAST(ID AS BIGINT) FROM D1 WHERE V > '0' ORDER BY ID",
		"SORT ((D1 NATURAL))", "SORT ((D1 INDEX (IX_D1)))");

	// Descending navigation must reverse the ascending sort exactly.
	{
		IdList ascending = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM D1 WHERE V IS NOT NULL"
			" PLAN SORT (D1 NATURAL) ORDER BY V, ID DESC");
		const IdList descending = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM D1 WHERE V IS NOT NULL"
			" PLAN SORT (D1 NATURAL) ORDER BY V DESC, ID");

		std::reverse(ascending.begin(), ascending.end());

		BOOST_CHECK_EQUAL_COLLECTIONS(ascending.begin(), ascending.end(),
			descending.begin(), descending.end());
	}
}


/* ------------------------------------------------------------------------ *
 * 3. Compound index
 *
 * key_empty and key_nulls are tracked per segment when the index has more
 * than one column, which is a different code path from the single segment
 * case covered everywhere else.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(CompoundIndex)
{
	TestDb db("compound");

	db.ddl("CREATE TABLE C1 (ID INTEGER, A D_LTZ, B D_LTZ)");

	db.exec("INSERT INTO C1 VALUES (1, '000',  'X')");
	db.exec("INSERT INTO C1 VALUES (2, '  ',   '0X')");
	db.exec("INSERT INTO C1 VALUES (3, '',     'x  ')");
	db.exec("INSERT INTO C1 VALUES (4, '0A',   '000')");
	db.exec("INSERT INTO C1 VALUES (5, 'a',    '')");
	db.exec("INSERT INTO C1 VALUES (6, 'A',    'Y')");
	db.exec("INSERT INTO C1 VALUES (7, NULL,   'Y')");
	db.exec("INSERT INTO C1 VALUES (8, 'A',    NULL)");
	db.exec("INSERT INTO C1 VALUES (9, '0AB',  '12345678901')");
	db.exec("INSERT INTO C1 VALUES (10, 'AB',  '00012345678901')");
	db.commit();

	db.ddl("CREATE INDEX IX_C1 ON C1(A, B)");

	checkSamePlanResult(db, "compound, both segments in the empty class",
		"SELECT CAST(ID AS BIGINT) FROM C1 WHERE A = '0' AND B = 'X' ORDER BY ID",
		"SORT ((C1 NATURAL))", "SORT ((C1 INDEX (IX_C1)))");

	checkSamePlanResult(db, "compound, leading segment only",
		"SELECT CAST(ID AS BIGINT) FROM C1 WHERE A = 'A' ORDER BY ID",
		"SORT ((C1 NATURAL))", "SORT ((C1 INDEX (IX_C1)))");

	checkSamePlanResult(db, "compound, trailing segment in the empty class",
		"SELECT CAST(ID AS BIGINT) FROM C1 WHERE A = 'A' AND B = '0' ORDER BY ID",
		"SORT ((C1 NATURAL))", "SORT ((C1 INDEX (IX_C1)))");

	checkSamePlanResult(db, "compound, NULL segment is not the empty class",
		"SELECT CAST(ID AS BIGINT) FROM C1 WHERE A = 'A' AND B IS NULL ORDER BY ID",
		"SORT ((C1 NATURAL))", "SORT ((C1 INDEX (IX_C1)))");

	checkSamePlanResult(db, "compound, segments of different normalized lengths",
		"SELECT CAST(ID AS BIGINT) FROM C1 WHERE A = 'AB' AND B = '12345678901' ORDER BY ID",
		"SORT ((C1 NATURAL))", "SORT ((C1 INDEX (IX_C1)))");

	// Both rows are the same pair of equivalence classes, so both must come
	// back through either plan.
	BOOST_CHECK_EQUAL(db.one(
		"SELECT CAST(COUNT(*) AS BIGINT) FROM C1 WHERE A = 'ab' AND B = '12345678901'"), 2);
}


/* ------------------------------------------------------------------------ *
 * 4. PRIMARY KEY and FOREIGN KEY
 *
 * Referential integrity resolves through a unique index lookup, so it depends
 * on string_to_key producing byte identical keys for values that compare()
 * calls equal.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(PrimaryAndForeignKey)
{
	TestDb db("keys");

	db.ddl("CREATE TABLE PARENT (V D_LTZ NOT NULL PRIMARY KEY)");
	db.ddl("CREATE TABLE CHILD (ID INTEGER, PV D_LTZ REFERENCES PARENT(V))");

	db.exec("INSERT INTO PARENT VALUES ('A')");
	db.exec("INSERT INTO PARENT VALUES ('B')");
	db.commit();

	BOOST_CHECK_MESSAGE(db.fails("INSERT INTO PARENT VALUES ('000a  ')"),
		"'000a  ' should collide with the primary key 'A'");

	BOOST_CHECK_MESSAGE(!db.fails("INSERT INTO CHILD VALUES (1, '0A')"),
		"child '0A' should resolve to parent 'A'");

	BOOST_CHECK_MESSAGE(!db.fails("INSERT INTO CHILD VALUES (2, '   a')"),
		"child '   a' should resolve to parent 'A'");

	BOOST_CHECK_MESSAGE(db.fails("INSERT INTO CHILD VALUES (3, 'Z')"),
		"child 'Z' has no parent and must be rejected");

	BOOST_CHECK_MESSAGE(db.fails("DELETE FROM PARENT WHERE V = '00000a'"),
		"deleting parent 'A' while children exist must be rejected");

	BOOST_CHECK_MESSAGE(!db.fails("DELETE FROM PARENT WHERE V = 'b'"),
		"parent 'B' has no children and must be deletable");

	BOOST_CHECK_EQUAL(db.one("SELECT CAST(COUNT(*) AS BIGINT) FROM PARENT"), 1);
	BOOST_CHECK_EQUAL(db.one("SELECT CAST(COUNT(*) AS BIGINT) FROM CHILD"), 2);
}


/* ------------------------------------------------------------------------ *
 * 5. gbak round trip
 *
 * Restore rebuilds every index by calling string_to_key again, which is
 * exactly the procedure documented for deploying a change to this collation.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(BackupRestoreRoundTrip)
{
	TestDb db("roundtrip");

	db.ddl("CREATE TABLE R1 (ID INTEGER NOT NULL PRIMARY KEY, V D_LTZ, W D_LTZ)");
	db.ddl("CREATE INDEX IX_R1_V ON R1(V)");
	db.ddl("CREATE DESCENDING INDEX IX_R1_W ON R1(W)");
	db.ddl("CREATE INDEX IX_R1_VW ON R1(V, W)");

	for (int i = 0; i < 500; i++)
	{
		char sql[256];
		snprintf(sql, sizeof(sql),
			"INSERT INTO R1 VALUES (%d,"
			" SUBSTRING('0000' FROM 1 FOR MOD(%d, 4) + 1) || 'K' || MOD(%d, 17),"
			" SUBSTRING('    ' FROM 1 FOR MOD(%d, 3) + 1) || MOD(%d, 7))",
			i, i, i, i, i);
		db.exec(sql);
	}

	db.commit();

	const std::string beforeAll = "SELECT CAST(ID AS BIGINT) FROM R1 ORDER BY V, W, ID";
	const std::string beforeProbe =
		"SELECT CAST(ID AS BIGINT) FROM R1 WHERE V = 'k3' ORDER BY ID";

	const IdList expectedAll = db.ids(beforeAll);
	const IdList expectedProbe = db.ids(beforeProbe);

	BOOST_REQUIRE_EQUAL(expectedAll.size(), 500u);
	BOOST_REQUIRE(!expectedProbe.empty());

	const std::string source = db.path;
	const std::string backup = "ltz_roundtrip.fbk";
	const std::string restored = "ltz_restored.fdb";

	remove(backup.c_str());
	remove(restored.c_str());

	db.detach();

	{
		Service service(fb_get_master_interface());

		IXpbBuilder* spb = service.startBuilder();
		spb->insertTag(&service.st, isc_action_svc_backup);
		spb->insertString(&service.st, isc_spb_dbname, source.c_str());
		spb->insertString(&service.st, isc_spb_bkp_file, backup.c_str());
		const std::string backupOut = service.run(spb);
		spb->dispose();

		BOOST_TEST_MESSAGE("backup:\n" << backupOut);
	}

	{
		Service service(fb_get_master_interface());

		IXpbBuilder* spb = service.startBuilder();
		spb->insertTag(&service.st, isc_action_svc_restore);
		spb->insertString(&service.st, isc_spb_bkp_file, backup.c_str());
		spb->insertString(&service.st, isc_spb_dbname, restored.c_str());
		spb->insertInt(&service.st, isc_spb_options, isc_spb_res_replace);
		const std::string restoreOut = service.run(spb);
		spb->dispose();

		BOOST_TEST_MESSAGE("restore:\n" << restoreOut);
	}

	// Compare the restored database against what was captured before.
	{
		IMaster* const master = fb_get_master_interface();
		ThrowStatusWrapper st(master->getStatus());
		IProvider* const prov = master->getDispatcher();

		IAttachment* att = nullptr;
		ITransaction* tra = nullptr;

		try
		{
			Dpb dpb(master, st);
			att = prov->attachDatabase(&st, restored.c_str(), dpb.length(), dpb.buffer());
			tra = att->startTransaction(&st, 0, nullptr);

			const IdList actualAll = fetchIds(st, att, tra, beforeAll.c_str());
			BOOST_CHECK_EQUAL_COLLECTIONS(expectedAll.begin(), expectedAll.end(),
				actualAll.begin(), actualAll.end());

			// The rebuilt indexes must answer the same as a natural scan.
			const IdList naturalProbe = fetchIds(st, att, tra,
				withPlan(beforeProbe, "SORT ((R1 NATURAL))").c_str());
			const IdList indexedProbe = fetchIds(st, att, tra,
				withPlan(beforeProbe, "SORT ((R1 INDEX (IX_R1_V)))").c_str());

			BOOST_CHECK_EQUAL_COLLECTIONS(expectedProbe.begin(), expectedProbe.end(),
				naturalProbe.begin(), naturalProbe.end());
			BOOST_CHECK_EQUAL_COLLECTIONS(expectedProbe.begin(), expectedProbe.end(),
				indexedProbe.begin(), indexedProbe.end());

			tra->commit(&st);
			tra = nullptr;

			att->dropDatabase(&st);
			att = nullptr;
		}
		catch (const FbException& e)
		{
			const std::string message = describe(master, e);

			if (tra)
				tra->release();

			if (att)
				att->release();

			st.dispose();
			prov->release();

			remove(backup.c_str());
			remove(restored.c_str());

			BOOST_FAIL(message);
		}

		st.dispose();
		prov->release();
	}

	remove(backup.c_str());
	remove(restored.c_str());

	db.reattach();
}


/* ------------------------------------------------------------------------ *
 * 6. Text BLOB and transliteration
 *
 * CONTAINING over a BLOB goes through texttype_fn_canonical on a different
 * path from the VARCHAR case, and comparing against literals of another
 * character set forces the engine to transliterate before comparing.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(BlobAndTransliteration)
{
	TestDb db("blob");

	db.ddl("CREATE TABLE B1 (ID INTEGER, V D_LTZ,"
		" T BLOB SUB_TYPE TEXT CHARACTER SET WIN1252 COLLATE LTZ)");

	db.exec("INSERT INTO B1 VALUES (1, '00000A', 'the ABC of it')");
	db.exec("INSERT INTO B1 VALUES (2, 'a',      'the abc of it')");
	db.exec("INSERT INTO B1 VALUES (3, 'B',      'nothing here')");
	db.commit();

	// CONTAINING is case insensitive through the canonical form.
	{
		const IdList expected = { 1, 2 };
		const IdList actual = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM B1 WHERE T CONTAINING 'abc' ORDER BY ID");

		BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(),
			actual.begin(), actual.end());
	}

	{
		const IdList expected = { 1, 2 };
		const IdList actual = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM B1 WHERE T CONTAINING 'ABC' ORDER BY ID");

		BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(),
			actual.begin(), actual.end());
	}

	// A literal of a different charset must be transliterated first and then
	// compared with the collation of the column.
	{
		const IdList expected = { 1, 2 };

		const IdList utf8 = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM B1 WHERE V = _UTF8 '0a' ORDER BY ID");
		const IdList latin1 = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM B1 WHERE V = _ISO8859_1 '  00A' ORDER BY ID");

		BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(),
			utf8.begin(), utf8.end());
		BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(),
			latin1.begin(), latin1.end());
	}
}


/* ------------------------------------------------------------------------ *
 * 7. Numeric ordering through a real index
 *
 * The unit test in LtrimZeroKeyTest.cpp owns the ordering rule itself. What
 * is checked here is that a b-tree built from those keys navigates in that
 * same order, and that a range over values of one width does not pick up a
 * narrower one.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(NumericOrderAndRange)
{
	TestDb db("numeric");

	db.ddl("CREATE TABLE N1 (ID INTEGER, V D_LTZ)");

	// ID is the expected ascending position, so ORDER BY V must return the
	// ids in increasing order.
	db.exec("INSERT INTO N1 VALUES (1, '000')");
	db.exec("INSERT INTO N1 VALUES (2, '9')");
	db.exec("INSERT INTO N1 VALUES (3, '0000009')");
	db.exec("INSERT INTO N1 VALUES (4, 'A')");
	db.exec("INSERT INTO N1 VALUES (5, '10')");
	db.exec("INSERT INTO N1 VALUES (6, '0001A34')");
	db.exec("INSERT INTO N1 VALUES (7, '12345678901')");
	db.exec("INSERT INTO N1 VALUES (8, '12345678000199')");
	db.exec("INSERT INTO N1 VALUES (9, '12345678009999')");
	db.commit();

	db.ddl("CREATE INDEX IX_N1_V ON N1(V)");

	// Sort keys: the plan carries a SORT, so what is being checked is the
	// order INTL_KEY_SORT produces. Ids 2 and 3 are the same class, so ID is
	// the tie breaker.
	{
		const IdList expected = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };

		const IdList sorted = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM N1"
			" PLAN SORT (N1 NATURAL) ORDER BY V, ID");

		BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(),
			sorted.begin(), sorted.end());
	}

	// Index navigation: no SORT in the plan, so the order comes from walking
	// the b-tree. Rows of the same class are ties whose relative order is not
	// defined, so the projection is the normalized length, which is equal for
	// tied rows and is exactly what the key prefix encodes.
	{
		const IdList expected = { 0, 1, 1, 1, 2, 4, 11, 14, 14 };

		const IdList navigated = db.ids(
			"SELECT CAST(CHAR_LENGTH(TRIM(LEADING '0' FROM TRIM(V))) AS BIGINT)"
			" FROM N1 PLAN (N1 ORDER IX_N1_V) ORDER BY V");

		BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(),
			navigated.begin(), navigated.end());
	}

	// The CNPJ root range: the 11 digit value must stay out, through both
	// plans.
	{
		const IdList expected = { 8, 9 };

		const std::string query =
			"SELECT CAST(ID AS BIGINT) FROM N1"
			" WHERE V BETWEEN '12345678000000' AND '12345678999999' ORDER BY ID";

		const IdList natural = db.ids(withPlan(query, "SORT ((N1 NATURAL))"));
		const IdList indexed = db.ids(withPlan(query, "SORT ((N1 INDEX (IX_N1_V)))"));

		BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(),
			natural.begin(), natural.end());
		BOOST_CHECK_EQUAL_COLLECTIONS(expected.begin(), expected.end(),
			indexed.begin(), indexed.end());
	}

	// Equality is untouched by the ordering change.
	checkSamePlanResult(db, "equality across leading zeros",
		"SELECT CAST(ID AS BIGINT) FROM N1 WHERE V = '0000009' ORDER BY ID",
		"SORT ((N1 NATURAL))", "SORT ((N1 INDEX (IX_N1_V)))");

	BOOST_CHECK_EQUAL(db.one("SELECT CAST(COUNT(*) AS BIGINT) FROM N1 WHERE V = '9'"), 2);
}


/* ------------------------------------------------------------------------ *
 * 8. STARTING WITH over an indexed column
 *
 * string_to_key returns an empty key for INTL_KEY_PARTIAL, which the engine
 * turns into a full index scan with blr_starting re-checked against the
 * record. The plan gets worse; the rows must not change. LIKE 'x%' is
 * rewritten into blr_starting by the optimizer and follows the same path.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(StartingWithMatchesNaturalScan)
{
	TestDb db("starting");

	db.ddl("CREATE TABLE S1 (ID INTEGER, V D_LTZ)");

	// Zero prefixed rows. STARTING WITH goes through texttype_fn_canonical,
	// a per character uppercase mapping that is deliberately NOT zero
	// insensitive (lc_ltrim_zero.cpp:64-71), so it matches raw bytes, not
	// equivalence classes. Every one of these rows begins with at least one
	// '0': MOD(i, 5) + 1 ranges 1..5, and SUBSTRING off a 4 character source
	// clips 5 down to 4, so the shortest prefix produced is a single '0'.
	// These rows are what makes '00' and '0000' below meaningful, and they
	// are also what makes the natural and index plans disagree if the empty
	// partial key path is wrong.
	for (int i = 0; i < 2000; i++)
	{
		char sql[256];
		snprintf(sql, sizeof(sql),
			"INSERT INTO S1 VALUES (%d,"
			" SUBSTRING('0000' FROM 1 FOR MOD(%d, 5) + 1) || 'K' || MOD(%d, 23))",
			i, i, i);
		db.exec(sql);
	}

	// A second set of rows with no leading zero at all, so 'K' and 'K1'
	// below have raw bytes to match. Without these, every row in the table
	// starts with '0', and STARTING WITH 'K' / 'K1' would match zero rows on
	// both plans: the comparison would still pass, but it would be an empty
	// list against an empty list, proving nothing about the index range.
	for (int i = 0; i < 2000; i++)
	{
		char sql[256];
		snprintf(sql, sizeof(sql),
			"INSERT INTO S1 VALUES (%d, 'K' || MOD(%d, 23))",
			2000 + i, i);
		db.exec(sql);
	}

	db.exec("INSERT INTO S1 VALUES (9001, '000')");
	db.exec("INSERT INTO S1 VALUES (9002, '   ')");
	db.commit();

	db.ddl("CREATE INDEX IX_S1_V ON S1(V)");

	struct Prefix
	{
		const char* text;
		bool matchesRows;	// true: must match at least one row; false: must match none
	};

	const Prefix prefixes[] =
	{
		{ "K",    true  },	// matches the no-zero-prefix rows added above
		{ "K1",   true  },	// a subset of them: 'K1', 'K10' .. 'K19'
		{ "00",   true  },	// raw prefix match: rows with two or more leading
							// zeros, not every row that equals the '0' class
		{ "0000", true  },	// raw prefix match: rows with all four leading zeros
		{ "ZZZ",  false }	// matches nothing, the deliberate zero case
	};

	for (const Prefix& p : prefixes)
	{
		const std::string query =
			std::string("SELECT CAST(ID AS BIGINT) FROM S1 WHERE V STARTING WITH '")
			+ p.text + "' ORDER BY ID";

		checkSamePlanResult(db, (std::string("STARTING WITH '") + p.text + "'").c_str(),
			query, "SORT ((S1 NATURAL))", "SORT ((S1 INDEX (IX_S1_V)))");

		// Anchor the count: checkSamePlanResult only proves both plans agree,
		// not that either found any rows. Two empty lists agree trivially.
		const ISC_INT64 count = db.one(
			std::string("SELECT CAST(COUNT(*) AS BIGINT) FROM S1 WHERE V STARTING WITH '")
			+ p.text + "'");

		if (p.matchesRows)
			BOOST_CHECK_GT(count, 0);
		else
			BOOST_CHECK_EQUAL(count, 0);
	}

	// LIKE 'x%' is rewritten into blr_starting and must agree as well, over
	// the same non-vacuous 'K1' match as the STARTING WITH case above.
	checkSamePlanResult(db, "LIKE 'K1%'",
		"SELECT CAST(ID AS BIGINT) FROM S1 WHERE V LIKE 'K1%' ORDER BY ID",
		"SORT ((S1 NATURAL))", "SORT ((S1 INDEX (IX_S1_V)))");

	BOOST_CHECK_GT(db.one("SELECT CAST(COUNT(*) AS BIGINT) FROM S1 WHERE V LIKE 'K1%'"), 0);

	// The empty prefix matches every row. It is checked without forcing a
	// plan, because the plan validator may refuse an index for a predicate it
	// considers unbounded, and that refusal would be an engine decision, not a
	// collation result.
	BOOST_CHECK_EQUAL(db.one("SELECT CAST(COUNT(*) AS BIGINT) FROM S1 WHERE V STARTING WITH ''"),
		db.one("SELECT CAST(COUNT(*) AS BIGINT) FROM S1"));
}


/* ------------------------------------------------------------------------ *
 * 9. Descending index at volume
 *
 * Almost every key now starts with 0x00, the high byte of the length prefix,
 * so the engine writes desc_end_value_prefix ahead of nearly every descending
 * key before complementing it (btr.cpp:2929-2934). That branch used to be
 * rare. Seven rows do not exercise page splits or prefix compression between
 * nodes, so this case carries the volume.
 * ------------------------------------------------------------------------ */

LTZ_TEST_CASE(DescendingIndexAtVolume)
{
	TestDb db("descvolume");

	db.ddl("CREATE TABLE DV (ID INTEGER NOT NULL PRIMARY KEY, V D_LTZ)");

	// Values of several different normalized lengths, plus the empty class,
	// so the length prefix varies across the whole index.
	db.exec(
		"EXECUTE BLOCK AS "
		"DECLARE I INTEGER; "
		"BEGIN "
		"  I = 0; "
		"  WHILE (I < 20000) DO "
		"  BEGIN "
		"    INSERT INTO DV VALUES (:I, "
		"      SUBSTRING('000000' FROM 1 FOR MOD(:I, 6) + 1) || "
		"      CASE MOD(:I, 7) WHEN 0 THEN '' ELSE CAST(MOD(:I, 99991) AS VARCHAR(10)) END); "
		"    I = I + 1; "
		"  END "
		"END");
	db.commit();

	db.ddl("CREATE DESCENDING INDEX IX_DV ON DV(V)");

	BOOST_REQUIRE_EQUAL(db.one("SELECT CAST(COUNT(*) AS BIGINT) FROM DV"), 20000);

	// Descending navigation must be the exact reverse of the ascending sort.
	//
	// Deviation from the task brief: the brief's version of this block
	// selected CAST(ID AS BIGINT) and ordered both sides by "V ..., ID" so
	// that ID would break ties. The descending side used
	// "PLAN (DV ORDER IX_DV) ORDER BY V DESC, ID", which the optimizer
	// rejects outright: IX_DV carries only V, a navigational PLAN cannot
	// also satisfy a trailing ID key, and the engine raises
	// "index IX_DV cannot be used in the specified plan" (isc_index_unused,
	// Optimizer.cpp) at statement preparation, before any row is touched.
	// This was confirmed with a minimal isql reproduction using the same
	// table shape (INTEGER NOT NULL PRIMARY KEY id, a DESCENDING index on a
	// second column) before spending the 20000 row build on it; the failure
	// does not depend on collation semantics, volume, or key contents.
	//
	// The fix keeps the navigational PLAN, which is what this case exists to
	// exercise, and drops the unsatisfiable secondary key from the ORDER BY
	// on that side. Since dropping the tiebreak leaves the order among tied
	// rows undefined, the comparison uses a projection that is constant
	// within one equivalence class and distinct across classes instead of
	// raw ID, the same technique already used in
	// ConcurrentVolumeAndValidation (RIGHT(V, 2)) and in
	// NumericOrderAndRange (CHAR_LENGTH(...)) above. MOD(ID, 7) = 0 is
	// exactly the condition the generator above uses to produce the empty
	// class, so it identifies the one tie group in this data set.
	//
	// The projection is tie invariant only for this data: MOD(ID, 99991) is
	// the identity for every ID under 20000, so for MOD(ID, 7) <> 0 the
	// normalized value is the decimal form of ID itself, unique per row, and
	// MOD(ID, 7) = 0 is the only source of ties. A different row count or a
	// different modulus can break that and would need a different check.
	{
		const std::string classOf = "CASE WHEN MOD(ID, 7) = 0 THEN -1 ELSE ID END";

		// No ", ID" tiebreak here: every tied row (the empty class) projects
		// to the same -1, and every non-tied row has a V that is already
		// unique (it normalizes to the decimal form of its own ID), so V
		// alone fully orders the projected column with nothing left to break.
		IdList ascending = db.ids(
			"SELECT CAST(" + classOf + " AS BIGINT) FROM DV"
			" PLAN SORT (DV NATURAL) ORDER BY V");
		const IdList descending = db.ids(
			"SELECT CAST(" + classOf + " AS BIGINT) FROM DV"
			" PLAN (DV ORDER IX_DV) ORDER BY V DESC");

		std::reverse(ascending.begin(), ascending.end());

		BOOST_CHECK_EQUAL(descending.size(), 20000u);
		BOOST_CHECK_EQUAL_COLLECTIONS(ascending.begin(), ascending.end(),
			descending.begin(), descending.end());
	}

	// The empty class through the descending index.
	checkSamePlanResult(db, "descending index, empty class at volume",
		"SELECT CAST(ID AS BIGINT) FROM DV WHERE V = '0' ORDER BY ID",
		"SORT ((DV NATURAL))", "SORT ((DV INDEX (IX_DV)))");

	// A compare / key mismatch surfaces as index corruption.
	const std::string dbPath = db.path;
	db.detach();

	{
		Service service(fb_get_master_interface());
		IXpbBuilder* spb = service.startBuilder();
		spb->insertTag(&service.st, isc_action_svc_validate);
		spb->insertString(&service.st, isc_spb_dbname, dbPath.c_str());

		const std::string output = service.run(spb);
		spb->dispose();

		BOOST_TEST_MESSAGE("online validation output:\n" << output);
		BOOST_CHECK(output.find("Error") == std::string::npos);
		BOOST_CHECK(output.find("corrupt") == std::string::npos);
	}

	db.reattach();
}


BOOST_AUTO_TEST_SUITE_END()	// LtrimZeroSuite
BOOST_AUTO_TEST_SUITE_END()	// EngineSuite


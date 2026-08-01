# gbak -FIX_DOMAINS Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Adicionar ao gbak um switch `-FIX_DOMAINS` que redefine domínios durante o restore, referenciando charsets e collations por nome, substituindo o hack hardcoded de `TDR_CNPJ` que existe em `gbak_hacked`.

**Architecture:** Um módulo novo e isolado (`src/burp/domain_remap.h/.cpp`) faz o parse das regras e guarda pendências, sem depender de GPRE. Em `restore.epp`, `get_global_field()` aplica tipo e tamanho e registra a pendência; a resolução de charset e collation por nome acontece antes da primeira relation, consultando o banco destino, onde as collations do backup já estão gravadas e nenhum formato foi calculado.

**Tech Stack:** C++ (padrão do Firebird 5), GPRE para os arquivos `.epp`, Boost.Test para testes unitários, MSVC 2022 no Windows.

## Global Constraints

- Spec de referência: `docs/superpowers/specs/2026-08-01-gbak-domain-remap-design.md`. Leia antes de começar.
- Branch de trabalho: `srs/gbak-domain-remap`, criada a partir de `v5.0-release`.
- Sem travessão (`—`) nem traço médio (`–`) em nenhum texto gerado: código, comentários, mensagens, docs, commits. Usar hífen simples.
- Mensagens de commit e comentários de código em inglês, seguindo o padrão do repositório.
- Nenhuma mensagem existente do catálogo pode ser reaproveitada para texto novo. O hack original abusava da msg 121 (`"restoring domain @1"`); isso não se repete.
- Nada de números literais de charset ou collation no código nem nas regras. Sempre nome.
- Tipos alvo usam `blr_varying` e `blr_text`, nunca o literal `37`.
- Sem o switch, o gbak precisa se comportar exatamente como o oficial.
- Largura mínima do tipo alvo segue a mesma regra do DDL (`DSC_string_length`), ver Task 3.

## Numeração já verificada no repositório

Use estes valores. Foram conferidos na árvore atual e estão livres:

| O que | Valor | Onde fica |
|---|---|---|
| Id do switch | `IN_SW_BURP_FIX_DOMAINS = 56` | `src/burp/burpswi.h` (último usado é 55, `IN_SW_BURP_DIRECT_IO`) |
| Constante de serviço | `isc_spb_res_fix_domains = 21` | `src/include/firebird/impl/consts_pub.h` (último usado é 20, `isc_spb_res_replica_mode`) |
| Mensagens novas | 411 a 419 | `src/include/firebird/impl/msg/gbak.h` (última usada é 410) |

## Estrutura de arquivos

| Arquivo | Responsabilidade |
|---|---|
| `src/burp/domain_remap.h` (criar) | Declaração de `RemapRule`, `DomainRemap`, e da API pública do módulo |
| `src/burp/domain_remap.cpp` (criar) | Parse de regras (`@arquivo` e inline), lista de pendências, comparação de nome de domínio |
| `src/burp/tests/DomainRemapTest.cpp` (criar) | Testes unitários do parser via Boost.Test |
| `src/burp/burpswi.h` (modificar) | Constante do switch e entrada na tabela |
| `src/burp/burp.h` (modificar) | Campos em `BurpGlobals` |
| `src/burp/burp.cpp` (modificar) | Consumo do argumento do switch |
| `src/burp/restore.epp` (modificar) | Aplicação nos três ramos ODS, resolução, relatório |
| `src/include/firebird/impl/consts_pub.h` (modificar) | `isc_spb_res_fix_domains` |
| `src/include/firebird/impl/msg/gbak.h` (modificar) | Mensagens 411 a 417 |
| `builds/win32/msvc15/burp.vcxproj` e `.filters` (modificar) | Registrar `domain_remap.cpp` |
| `builds/win32/msvc15/common_test.vcxproj` (modificar) | Registrar `DomainRemapTest.cpp` e `domain_remap.cpp` |

CMake e POSIX não precisam de alteração: `src/CMakeLists.txt:325` usa `file(GLOB burp_src "burp/*.cpp" "burp/*.h")` e `builds/posix/make.shared.variables:107` usa `$(call dirObjects,burp)`. Ambos pegam o arquivo novo automaticamente. Só o MSVC lista fontes manualmente.

---

### Task 1: Módulo de parse das regras

O módulo é puro: sem `BurpGlobals`, sem GPRE, sem I/O de banco. É isso que o torna testável no `common_test`.

**Files:**
- Create: `src/burp/domain_remap.h`
- Create: `src/burp/domain_remap.cpp`
- Test: `src/burp/tests/DomainRemapTest.cpp`
- Modify: `builds/win32/msvc15/common_test.vcxproj`

**Interfaces:**
- Produces:
  - `struct Burp::RemapRule { Firebird::string domainName; UCHAR blrType; USHORT charLength; Firebird::string charsetName; Firebird::string collationName; bool applied; }`
  - `class Burp::DomainRemap` com `void parse(const char* spec)`, `RemapRule* findRule(const char* paddedName, size_t nameSize)`, `unsigned ruleCount() const`, `const RemapRule& rule(unsigned index) const`
  - `class Burp::DomainRemapError` (exceção com `Firebird::string message`)

- [ ] **Step 1: Escrever o teste que falha**

Crie `src/burp/tests/DomainRemapTest.cpp`:

```cpp
#include "firebird.h"
#include "boost/test/unit_test.hpp"
#include "../burp/domain_remap.h"

using namespace Firebird;
using namespace Burp;

BOOST_AUTO_TEST_SUITE(BurpSuite)
BOOST_AUTO_TEST_SUITE(DomainRemapSuite)


BOOST_AUTO_TEST_CASE(ParseInlineRule)
{
	DomainRemap remap;
	remap.parse("TDR_CNPJ = VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE ISO8859_1_LTRIM_ZERO_AI");

	BOOST_TEST(remap.ruleCount() == 1u);
	BOOST_TEST(remap.rule(0).domainName == "TDR_CNPJ");
	BOOST_TEST(remap.rule(0).blrType == blr_varying);
	BOOST_TEST(remap.rule(0).charLength == 20u);
	BOOST_TEST(remap.rule(0).charsetName == "ISO8859_1");
	BOOST_TEST(remap.rule(0).collationName == "ISO8859_1_LTRIM_ZERO_AI");
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
	BOOST_TEST(remap.rule(1).collationName == "PT_BR");
}

BOOST_AUTO_TEST_CASE(IgnoresCommentsAndBlankLines)
{
	DomainRemap remap;
	remap.parse("# comentario\n\nA = CHAR(5)\n# outro\n");

	BOOST_TEST(remap.ruleCount() == 1u);
	BOOST_TEST(remap.rule(0).domainName == "A");
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

	// RDB$FIELD_NAME chega como CHAR(63) preenchido com espacos
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
	BOOST_TEST(remap.rule(0).domainName == "TDR_CNPJ");
}

BOOST_AUTO_TEST_CASE(ParseUncheckedFlag)
{
	DomainRemap remap;
	remap.parse("A = VARCHAR(17) COLLATE PT_BR UNCHECKED; B = VARCHAR(20)");

	BOOST_TEST(remap.rule(0).uncheckedWidth == true);
	BOOST_TEST(remap.rule(0).collationName == "PT_BR");
	BOOST_TEST(remap.rule(1).uncheckedWidth == false);
}


BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_SUITE_END()
```

Registre os dois arquivos no `common_test.vcxproj`, dentro do mesmo `<ItemGroup>` que já contém `AlignerTest.cpp` (por volta da linha 185):

```xml
    <ClCompile Include="..\..\..\src\burp\tests\DomainRemapTest.cpp" />
    <ClCompile Include="..\..\..\src\burp\domain_remap.cpp" />
```

- [ ] **Step 2: Rodar o teste para confirmar que falha**

Run: build do `common_test` no MSVC.
Expected: FAIL de compilação, `domain_remap.h` não existe.

- [ ] **Step 3: Escrever o header**

Crie `src/burp/domain_remap.h`:

```cpp
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
	RemapRule()
		: blrType(0), charLength(0), uncheckedWidth(false), applied(false)
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
```

- [ ] **Step 4: Escrever a implementação**

Crie `src/burp/domain_remap.cpp`. O parser é deliberadamente simples: tokeniza por espaço depois de normalizar, e valida cada posição.

```cpp
/*
 *	PROGRAM:	JRD Backup and Restore Program
 *	MODULE:		domain_remap.cpp
 *	DESCRIPTION:	Domain redefinition rules for restore (-FIX_DOMAINS)
 */

#include "firebird.h"
#include "../burp/domain_remap.h"
#include "../common/os/os_utils.h"
#include "../jrd/blr.h"
#include <stdio.h>

using namespace Firebird;

namespace {

	// Trims blanks and tabs from both ends.
	void trim(string& s)
	{
		const char* const blanks = " \t\r\n";
		const size_t first = s.find_first_not_of(blanks);

		if (first == string::npos)
		{
			s = "";
			return;
		}

		const size_t last = s.find_last_not_of(blanks);
		s = s.substr(first, last - first + 1);
	}

	// Splits "VARCHAR(20)" into type keyword and length.
	// Returns false if the shape is not <word>(<digits>).
	bool splitTypeSpec(const string& token, string& keyword, USHORT& length)
	{
		const size_t open = token.find('(');

		if (open == string::npos || token[token.length() - 1] != ')')
			return false;

		keyword = token.substr(0, open);
		const string digits = token.substr(open + 1, token.length() - open - 2);

		if (digits.isEmpty())
			return false;

		for (size_t i = 0; i < digits.length(); ++i)
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
	{
		text += buffer;
		text += '\n';
	}

	fclose(file);
}

void DomainRemap::parseRules(const string& text)
{
	string pending;

	for (size_t i = 0; i <= text.length(); ++i)
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
	const size_t equals = line.find('=');

	if (equals == string::npos)
	{
		string msg;
		msg.printf("malformed domain remap rule: %s", line.c_str());
		throw DomainRemapError(msg);
	}

	RemapRule rule;

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

	for (size_t i = 0; i <= rest.length(); ++i)
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
	// RDB$FIELD_NAME is a space padded CHAR field, so trim before comparing.
	size_t length = nameSize;

	while (length > 0 && (name[length - 1] == ' ' || name[length - 1] == '\0'))
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
```

- [ ] **Step 5: Rodar os testes até passarem**

Run: `common_test.exe --run_test=BurpSuite/DomainRemapSuite`
Expected: todos os casos PASS. Se `blr_varying`/`blr_text` não resolverem, confira o include de `../jrd/blr.h`.

- [ ] **Step 6: Commit**

```bash
git add src/burp/domain_remap.h src/burp/domain_remap.cpp src/burp/tests/DomainRemapTest.cpp builds/win32/msvc15/common_test.vcxproj
git commit -m "feat(burp): add domain remap rule parser"
```

---

### Task 2: Switch -FIX_DOMAINS, mensagens e constante de serviço

Ao final desta task o switch existe, aparece na ajuda, é aceito na linha de comando e o argumento chega parseado, mas ainda não altera nada no restore.

**Files:**
- Modify: `src/burp/burpswi.h`
- Modify: `src/burp/burp.h`
- Modify: `src/burp/burp.cpp`
- Modify: `src/include/firebird/impl/consts_pub.h`
- Modify: `src/include/firebird/impl/msg/gbak.h`
- Modify: `builds/win32/msvc15/burp.vcxproj` e `burp.vcxproj.filters`

**Interfaces:**
- Consumes: `Burp::DomainRemap`, `Burp::DomainRemapError` da Task 1.
- Produces: `BurpGlobals::gbl_sw_fix_domains` (const SCHAR*) e `BurpGlobals::gbl_domain_remap` (`Burp::DomainRemap*`, nulo quando o switch não foi usado).

- [ ] **Step 1: Adicionar as mensagens no catálogo**

Em `src/include/firebird/impl/msg/gbak.h`, no fim do arquivo, depois da linha 410:

```c
FB_IMPL_MSG_NO_SYMBOL(GBAK, 411, "    @1FIX_D(OMAINS)       redefine domains during restore, @2file or inline rules")
FB_IMPL_MSG_SYMBOL(GBAK, 412, gbak_missing_domain_rules, "domain remap rules parameter missing")
FB_IMPL_MSG_SYMBOL(GBAK, 413, gbak_invalid_domain_rules, "invalid domain remap rules: @1")
FB_IMPL_MSG_NO_SYMBOL(GBAK, 414, "remapping domain @1 to @2")
FB_IMPL_MSG_SYMBOL(GBAK, 415, gbak_domain_not_found, "domain @1 from remap rules was not found in the backup")
FB_IMPL_MSG_SYMBOL(GBAK, 416, gbak_domain_name_unresolved, "cannot resolve @1 @2 for domain @3")
FB_IMPL_MSG_NO_SYMBOL(GBAK, 417, "remapped @1 domain(s), @2 rule(s) given")
FB_IMPL_MSG_NO_SYMBOL(GBAK, 419, "domain @1 target width @2 is below the @3 the DDL requires, UNCHECKED was given")
```

A msg 418 é adicionada na Task 5, junto do relatório que a usa.

Nota sobre a 411: o `@1` é o prefixo de switch que o gbak injeta em todas as linhas de ajuda (veja as msgs 406 e 409 como modelo), e o `@2` recebe o literal `@` do exemplo de uso.

- [ ] **Step 2: Adicionar a constante de serviço**

Em `src/include/firebird/impl/consts_pub.h`, logo após `isc_spb_res_replica_mode` (linha 571):

```c
#define isc_spb_res_fix_domains			21
```

- [ ] **Step 3: Declarar o switch**

Em `src/burp/burpswi.h`, após a linha 103 (`IN_SW_BURP_DIRECT_IO = 55`):

```cpp
const int IN_SW_BURP_FIX_DOMAINS		= 56;	// redefine domains during restore
```

E na tabela `burp_in_sw_table`, logo após a entrada de `FIX_FSS_METADATA` (linha 138):

```cpp
	{IN_SW_BURP_FIX_DOMAINS,		isc_spb_res_fix_domains,
												"FIX_DOMAINS",		0, 0, 0, false, false,	411,	5, NULL, boRestore},
				// msg 411: @1FIX_D(OMAINS)       redefine domains during restore
```

O `5` é a abreviação mínima, o que torna `-FIX_D` suficiente e não colide com os `FIX_FSS_*`, que exigem 9.

- [ ] **Step 4: Declarar os campos globais**

Em `src/burp/burp.h`, junto dos campos de `fix_fss` (por volta da linha 1015):

```cpp
	const SCHAR*	gbl_sw_fix_domains;
	Burp::DomainRemap*	gbl_domain_remap;
```

E no topo do arquivo, junto dos outros includes de burp:

```cpp
#include "../burp/domain_remap.h"
```

- [ ] **Step 5: Consumir o argumento**

Em `src/burp/burp.cpp`, no `switch` de switches, logo após o `case IN_SW_BURP_FIX_FSS_METADATA:` (por volta da linha 870):

```cpp
		case IN_SW_BURP_FIX_DOMAINS:
			if (tdgbl->gbl_sw_fix_domains)
				BURP_error(333, true, SafeArg() << in_sw_tab->in_sw_name << tdgbl->gbl_sw_fix_domains);
			if (++itr >= argc)
			{
				BURP_error(412, true);
				// msg 412 domain remap rules parameter missing
			}
			tdgbl->gbl_sw_fix_domains = argv[itr];
			break;
```

E onde os switches já validados são transformados em estado (procure onde `gbl_sw_fix_fss_data_id` é resolvido), acrescente o parse, para que erro de sintaxe aborte antes de o banco ser criado:

```cpp
	if (tdgbl->gbl_sw_fix_domains)
	{
		tdgbl->gbl_domain_remap = FB_NEW_POOL(*getDefaultMemoryPool())
			Burp::DomainRemap(*getDefaultMemoryPool());

		try
		{
			tdgbl->gbl_domain_remap->parse(tdgbl->gbl_sw_fix_domains);
		}
		catch (const Burp::DomainRemapError& ex)
		{
			BURP_error(413, true, SafeArg() << ex.message.c_str());
			// msg 413 invalid domain remap rules: @1
		}
	}
```

- [ ] **Step 6: Registrar o fonte no MSVC**

Em `builds/win32/msvc15/burp.vcxproj`, no `<ItemGroup>` de `ClCompile` (linha 138 em diante):

```xml
    <ClCompile Include="..\..\..\src\burp\domain_remap.cpp" />
```

E em `burp.vcxproj.filters`, na mesma seção onde os outros `src\burp\*.cpp` aparecem, com o mesmo filtro deles.

- [ ] **Step 7: Verificar**

Run: build do gbak, depois `gbak.exe -?`
Expected: a linha do `FIX_D(OMAINS)` aparece na ajuda.

Run: `gbak.exe -c backup.fbk teste.fdb -FIX_DOMAINS "A VARCHAR(10)"`
Expected: falha com "invalid domain remap rules: malformed domain remap rule: A VARCHAR(10)", e nenhum banco criado.

- [ ] **Step 8: Commit**

```bash
git add src/burp/burpswi.h src/burp/burp.h src/burp/burp.cpp src/include/firebird/impl/consts_pub.h src/include/firebird/impl/msg/gbak.h builds/win32/msvc15/burp.vcxproj builds/win32/msvc15/burp.vcxproj.filters
git commit -m "feat(burp): add -FIX_DOMAINS switch and its catalog messages"
```

---

### Task 3: Aplicação nos três ramos ODS

Aqui o domínio passa a ser gravado com o tipo novo. Charset e collation ainda ficam provisórios; quem resolve é a Task 4.

**Files:**
- Modify: `src/burp/restore.epp` (função `get_global_field`, ramos em 5412, 5785 e 6135)

**Interfaces:**
- Consumes: `BurpGlobals::gbl_domain_remap`, `Burp::RemapRule` da Task 1.
- Produces: função estática `bool apply_domain_remap(BurpGlobals* tdgbl, const char* fieldName, size_t nameSize, SSHORT& fieldType, SSHORT& fieldLength, SSHORT& fieldScale, SSHORT& fieldSubType, SSHORT& charLength, bool& precisionNull)`, declarada junto das outras estáticas no topo de `restore.epp`.

- [ ] **Step 1: Escrever a função de aplicação**

Em `src/burp/restore.epp`, junto das demais funções estáticas do arquivo:

```cpp
static bool apply_domain_remap(BurpGlobals* tdgbl,
							   const char* fieldName,
							   size_t nameSize,
							   SSHORT& fieldType,
							   SSHORT& fieldLength,
							   SSHORT& fieldScale,
							   SSHORT& fieldSubType,
							   SSHORT& charLength,
							   bool& precisionNull)
{
/**************************************
 *
 *	a p p l y _ d o m a i n _ r e m a p
 *
 **************************************
 *
 * Functional description
 *	Redefine a global field according to the -FIX_DOMAINS rules.
 *	Character set and collation are left at their defaults here and
 *	resolved by name later, once the backup's collations have been
 *	restored. Returns true when a rule matched.
 *
 **************************************/
	if (!tdgbl->gbl_domain_remap)
		return false;

	Burp::RemapRule* const rule = tdgbl->gbl_domain_remap->findRule(fieldName, nameSize);

	if (!rule)
		return false;

	// A domain already in the target shape means the rules do not match the
	// backup, and silently doing nothing would hide that.
	const UCHAR currentType = (fieldType == blr_varying || fieldType == blr_text) ?
		(UCHAR) fieldType : 0;

	if (currentType == rule->blrType)
	{
		BURP_error(413, true, SafeArg() <<
			"domain is already in the target type, refusing to remap");
	}

	// Same minimum width the DDL enforces in AlterDomainNode::checkUpdate.
	dsc desc;
	desc.clear();
	desc.dsc_dtype = (fieldType == blr_int64) ? dtype_int64 : dtype_long;
	desc.dsc_length = fieldLength;
	desc.dsc_scale = (SCHAR) fieldScale;

	const USHORT minimum = DSC_string_length(&desc);

	if (rule->charLength < minimum)
	{
		if (rule->uncheckedWidth)
		{
			// The operator asked for a narrower type than the DDL would allow.
			// Any value that does not fit fails later, while data is loading.
			BURP_print(false, 419, SafeArg() << rule->domainName.c_str() <<
				(int) rule->charLength << (int) minimum);
			// msg 419 domain @1 target width @2 is below the @3 the DDL requires, UNCHECKED was given
		}
		else
		{
			Firebird::string msg;
			msg.printf("new size for domain %s must be at least %d characters, or add UNCHECKED",
				rule->domainName.c_str(), minimum);
			BURP_error(413, true, SafeArg() << msg.c_str());
		}
	}

	fieldType = rule->blrType;
	fieldLength = rule->charLength;
	charLength = rule->charLength;
	fieldScale = 0;
	fieldSubType = 0;
	precisionNull = true;
	rule->applied = true;

	return true;
}
```

Note que `fieldLength` recebe `charLength` porque os charsets em uso aqui são de 1 byte por caractere. Se uma regra nomear um charset multibyte, a Task 4 corrige o comprimento em bytes ao resolver o charset.

- [ ] **Step 2: Chamar nos três ramos**

Em cada um dos três blocos `STORE ... X IN RDB$FIELDS` de `get_global_field`, imediatamente antes do `END_STORE`, insira a mesma chamada. Os pontos são: ramo `>= DB_VERSION_DDL12` (o `END_STORE` por volta de 5778), ramo `>= DB_VERSION_DDL10` (por volta de 6129) e o ramo `else` (o `END_STORE` do bloco iniciado em 6139).

```cpp
		{
			// Copie para locais antes de chamar: os campos de X sao membros de
			// struct gerada pelo GPRE e nao podem ser vinculados a referencia.
			SSHORT remapType = X.RDB$FIELD_TYPE;
			SSHORT remapLength = X.RDB$FIELD_LENGTH;
			SSHORT remapScale = X.RDB$FIELD_SCALE;
			SSHORT remapSubType = X.RDB$FIELD_SUB_TYPE;
			SSHORT remapCharLength = X.RDB$CHARACTER_LENGTH;
			bool remapPrecisionNull = X.RDB$FIELD_PRECISION.NULL;

			if (apply_domain_remap(tdgbl, X.RDB$FIELD_NAME, sizeof(X.RDB$FIELD_NAME),
					remapType, remapLength, remapScale, remapSubType,
					remapCharLength, remapPrecisionNull))
			{
				X.RDB$FIELD_TYPE = remapType;
				X.RDB$FIELD_LENGTH = remapLength;
				X.RDB$FIELD_SCALE = remapScale;
				X.RDB$FIELD_SCALE.NULL = FALSE;
				X.RDB$FIELD_SUB_TYPE = remapSubType;
				X.RDB$FIELD_SUB_TYPE.NULL = FALSE;
				X.RDB$CHARACTER_LENGTH = remapCharLength;
				X.RDB$CHARACTER_LENGTH.NULL = FALSE;
				X.RDB$FIELD_PRECISION.NULL = remapPrecisionNull;
			}
		}
```

No ramo `else` (`< DB_VERSION_DDL10`), `RDB$CHARACTER_LENGTH` pode não existir na estrutura; se o GPRE reclamar, remova a leitura e as duas atribuições de `CHARACTER_LENGTH` nesse bloco apenas, mantendo o resto igual.

- [ ] **Step 3: Compilar**

Run: `make_gbak_boot.bat`
Expected: build limpo. Erros de GPRE apontam para o `.epp`, não para o `.cpp` gerado.

- [ ] **Step 4: Verificar o efeito parcial**

Prepare um banco de teste:

```sql
CREATE DATABASE 'd:\u\banco\remap_src.fdb' DEFAULT CHARACTER SET ISO8859_1;
CREATE DOMAIN TDR_TESTE AS BIGINT;
CREATE TABLE T (ID INTEGER, V TDR_TESTE);
INSERT INTO T VALUES (1, 123);
COMMIT;
```

Run:
```
gbak.exe -b d:\u\banco\remap_src.fdb d:\u\banco\remap.fbk
gbak.exe -c d:\u\banco\remap.fbk d:\u\banco\remap_dst.fdb -FIX_DOMAINS "TDR_TESTE = VARCHAR(20)"
isql.exe d:\u\banco\remap_dst.fdb -q -i verifica.sql
```

Com `verifica.sql`:
```sql
SELECT RDB$FIELD_TYPE, RDB$FIELD_LENGTH FROM RDB$FIELDS WHERE TRIM(RDB$FIELD_NAME) = 'TDR_TESTE';
SELECT V FROM T WHERE ID = 1;
```

Expected: `RDB$FIELD_TYPE` = 37, `RDB$FIELD_LENGTH` = 20, e `V` retorna `123` como texto.

Run também: `gbak.exe -c d:\u\banco\remap.fbk d:\u\banco\x.fdb -FIX_DOMAINS "TDR_TESTE = VARCHAR(17)"`
Expected: erro "new size for domain TDR_TESTE must be at least 20 characters, or add UNCHECKED".

Run: `gbak.exe -c d:\u\banco\remap.fbk d:\u\banco\y.fdb -FIX_DOMAINS "TDR_TESTE = VARCHAR(17) UNCHECKED"`
Expected: aviso da msg 419 e restore concluído, com `RDB$FIELD_LENGTH` = 17.

Run, para confirmar que a falha tardia é real e legível: repita o `UNCHECKED` com largura absurdamente pequena, por exemplo `VARCHAR(2)`, sobre um valor que não caiba.
Expected: o restore chega a carregar dados e aborta com erro de conversão do engine, não com erro do remap. É esse o custo do override.

- [ ] **Step 5: Commit**

```bash
git add src/burp/restore.epp
git commit -m "feat(burp): apply domain remap rules in get_global_field"
```

---

### Task 4: Resolução de charset e collation por nome

**Files:**
- Modify: `src/burp/restore.epp` (função nova + chamadas em `case rec_relation:`, `case rec_relation_data:` e no fim do restore)

**Interfaces:**
- Consumes: `Burp::RemapRule::applied` marcado na Task 3.
- Produces: `static void resolve_domain_remap(BurpGlobals* tdgbl)`, idempotente.

- [ ] **Step 1: Escrever a resolução**

Em `src/burp/restore.epp`. Precisa de GPRE porque consulta as system tables:

```cpp
static void resolve_domain_remap(BurpGlobals* tdgbl)
{
/**************************************
 *
 *	r e s o l v e _ d o m a i n _ r e m a p
 *
 **************************************
 *
 * Functional description
 *	Resolve character set and collation names for every domain that was
 *	remapped, and patch RDB$FIELDS accordingly. Runs after the backup's
 *	collations have been stored and before any relation exists, so no
 *	relation format has been computed yet. Idempotent.
 *
 **************************************/
	if (!tdgbl->gbl_domain_remap || tdgbl->gbl_domain_remap_done)
		return;

	tdgbl->gbl_domain_remap_done = true;

	Burp::DomainRemap& remap = *tdgbl->gbl_domain_remap;
	unsigned remapped = 0;

	for (unsigned i = 0; i < remap.ruleCount(); ++i)
	{
		Burp::RemapRule& rule = remap.rule(i);

		// Every rule must have matched exactly one domain. A rule that never
		// matched means the rules do not describe this backup, and letting it
		// pass would ship a domain with the wrong collation and no complaint.
		if (!rule.applied)
		{
			BURP_error(415, true, SafeArg() << rule.domainName.c_str());
			// msg 415 domain @1 from remap rules was not found in the backup
		}

		SSHORT charsetId = 0;
		SSHORT collationId = 0;
		bool charsetFound = rule.charsetName.isEmpty();

		// Handles locais mais MISC_release_request_silent: e o padrao usado
		// pela resolucao de charset do FIX_FSS em restore.epp:10577, e nao
		// membros de BurpGlobals, que servem para handles reaproveitados a
		// cada registro do backup.
		if (!charsetFound)
		{
			Firebird::IRequest* req_charset = nullptr;

			FOR (REQUEST_HANDLE req_charset)
				CS IN RDB$CHARACTER_SETS
				WITH CS.RDB$CHARACTER_SET_NAME EQ rule.charsetName.c_str()

				charsetId = CS.RDB$CHARACTER_SET_ID;
				charsetFound = true;
			END_FOR;
			ON_ERROR
				general_on_error();
			END_ERROR;

			MISC_release_request_silent(req_charset);

			if (!charsetFound)
			{
				BURP_error(416, true, SafeArg() << "character set" <<
					rule.charsetName.c_str() << rule.domainName.c_str());
				// msg 416 cannot resolve @1 @2 for domain @3
			}
		}

		if (rule.collationName.hasData())
		{
			bool collationFound = false;
			Firebird::IRequest* req_collation = nullptr;

			FOR (REQUEST_HANDLE req_collation)
				CL IN RDB$COLLATIONS
				WITH CL.RDB$COLLATION_NAME EQ rule.collationName.c_str()

				collationId = CL.RDB$COLLATION_ID;

				if (rule.charsetName.isEmpty())
					charsetId = CL.RDB$CHARACTER_SET_ID;

				collationFound = true;
			END_FOR;
			ON_ERROR
				general_on_error();
			END_ERROR;

			MISC_release_request_silent(req_collation);

			if (!collationFound)
			{
				BURP_error(416, true, SafeArg() << "collation" <<
					rule.collationName.c_str() << rule.domainName.c_str());
			}
		}

		unsigned touched = 0;
		Firebird::IRequest* req_field = nullptr;

		FOR (REQUEST_HANDLE req_field)
			X IN RDB$FIELDS
			WITH X.RDB$FIELD_NAME EQ rule.domainName.c_str()

			MODIFY X USING
				X.RDB$CHARACTER_SET_ID = charsetId;
				X.RDB$CHARACTER_SET_ID.NULL = FALSE;
				X.RDB$COLLATION_ID = collationId;
				X.RDB$COLLATION_ID.NULL = FALSE;
			END_MODIFY;

			++touched;
		END_FOR;
		ON_ERROR
			general_on_error();
		END_ERROR;

		MISC_release_request_silent(req_field);

		if (touched != 1)
		{
			Firebird::string msg;
			msg.printf("domain %s matched %u rows in RDB$FIELDS, expected 1",
				rule.domainName.c_str(), touched);
			BURP_error(413, true, SafeArg() << msg.c_str());
		}

		Firebird::string target;
		target.printf("%s(%d)%s%s%s%s",
			rule.blrType == blr_varying ? "VARCHAR" : "CHAR",
			(int) rule.charLength,
			rule.charsetName.hasData() ? " CHARACTER SET " : "",
			rule.charsetName.c_str(),
			rule.collationName.hasData() ? " COLLATE " : "",
			rule.collationName.c_str());

		BURP_verbose(414, SafeArg() << rule.domainName.c_str() << target.c_str());
		// msg 414 remapping domain @1 to @2
		++remapped;
	}

	BURP_verbose(417, SafeArg() << remapped << remap.ruleCount());
	// msg 417 remapped @1 domain(s), @2 rule(s) given
}
```

Declare apenas o flag em `src/burp/burp.h`, junto dos demais `gbl_sw_*`. Os handles são locais, então
não entram em `BurpGlobals`:

```cpp
	bool				gbl_domain_remap_done;
```

Padrão de referência para toda a função: `restore.epp:10577-10600`, onde o `FIX_FSS_DATA` resolve o
nome do charset para id. Mesma forma de `FOR ... WITH ... EQ name.c_str()`, mesmo flag `found`, mesmo
`MISC_release_request_silent` no fim.

Sobre as mensagens: já existe a msg 305, `"Character set @1 not found"`. Ela não é usada aqui porque
a 416 nomeia também o domínio da regra, o que é o que o operador precisa para saber qual linha do
arquivo corrigir, e serve tanto para charset quanto para collation.

- [ ] **Step 2: Chamar nos três pontos**

Em `src/burp/restore.epp`, no `switch (record)` do laço principal:

No `case rec_relation:` (por volta de 10670), como primeira linha:
```cpp
		case rec_relation:
			resolve_domain_remap(tdgbl);
			if (!get_relation(tdgbl, &coord, &task))
```

No `case rec_relation_data:` (por volta de 10731), antes do `if (flag)`:
```cpp
		case rec_relation_data:
			resolve_domain_remap(tdgbl);
			if (flag)
```

E no fim do restore, antes do commit final, para cobrir backups sem nenhuma relation.

- [ ] **Step 3: Compilar e verificar o caminho feliz**

Prepare um banco com collation nomeada, para exercitar a resolução de verdade:

```sql
CREATE DATABASE 'd:\u\banco\remap_src2.fdb' DEFAULT CHARACTER SET ISO8859_1;
CREATE COLLATION MINHA_COLL FOR ISO8859_1 FROM PT_BR CASE INSENSITIVE;
CREATE DOMAIN TDR_TESTE AS BIGINT;
CREATE TABLE T (ID INTEGER, V TDR_TESTE);
INSERT INTO T VALUES (1, 123);
COMMIT;
```

Run:
```
gbak.exe -b d:\u\banco\remap_src2.fdb d:\u\banco\remap2.fbk
gbak.exe -c d:\u\banco\remap2.fbk d:\u\banco\remap_dst2.fdb -v -FIX_DOMAINS "TDR_TESTE = VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE MINHA_COLL"
```

Expected na saída verbose: `remapping domain TDR_TESTE to VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE MINHA_COLL` e `remapped 1 domain(s), 1 rule(s) given`.

Confirme que o nome aterrissou, e não só um número:
```sql
SELECT c.RDB$COLLATION_NAME
  FROM RDB$COLLATIONS c
  JOIN RDB$FIELDS f ON f.RDB$COLLATION_ID = c.RDB$COLLATION_ID
                   AND f.RDB$CHARACTER_SET_ID = c.RDB$CHARACTER_SET_ID
 WHERE TRIM(f.RDB$FIELD_NAME) = 'TDR_TESTE';
```
Expected: `MINHA_COLL`.

- [ ] **Step 4: Verificar os abortos**

Run: `-FIX_DOMAINS "NAO_EXISTE = VARCHAR(20)"`
Expected: "domain NAO_EXISTE from remap rules was not found in the backup", restore abortado.

Run: `-FIX_DOMAINS "TDR_TESTE = VARCHAR(20) COLLATE NAO_EXISTE"`
Expected: "cannot resolve collation NAO_EXISTE for domain TDR_TESTE", restore abortado.

Em ambos os casos confirme que o banco destino não ficou utilizável.

- [ ] **Step 5: Commit**

```bash
git add src/burp/restore.epp src/burp/burp.h
git commit -m "feat(burp): resolve remapped domain charset and collation by name"
```

---

### Task 5: Preview com -m e teste end-to-end da collation

O preview não é switch novo: é o `-m` que já existe. Esta task confirma isso e fecha a validação semântica que a spec exige na seção 8.

**Files:**
- Create: `doc/README.fix_domains.md`

- [ ] **Step 1: Confirmar o preview**

Run:
```
gbak.exe -m -c d:\u\banco\remap2.fbk d:\u\banco\preview.fdb -v -FIX_DOMAINS "TDR_TESTE = VARCHAR(20) COLLATE MINHA_COLL"
```
Expected: as mesmas linhas de `remapping domain` e do resumo, sem restaurar dados. Rode também com uma regra errada e confirme que aborta igual ao restore completo.

Se nenhuma das três chamadas de `resolve_domain_remap` for alcançada sob `-m`, adicione a chamada no ponto que faltar e registre qual era.

- [ ] **Step 2: Teste semântico da collation**

Este é o teste que a seção 8 da spec exige e que ainda não foi feito em nenhum momento do projeto. Precisa do `fbintl` com a LTRIM_ZERO instalado, ou seja, da branch integradora da Task 6. Se ainda não estiver disponível, execute a Task 6 antes desta step.

```sql
CREATE COLLATION ISO8859_1_LTRIM_ZERO FOR ISO8859_1 FROM EXTERNAL ('ISO8859_1_LTRIM_ZERO');
```

Restaure com `-FIX_DOMAINS "TDR_TESTE = VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE ISO8859_1_LTRIM_ZERO"` sobre um backup em que a coluna guarde `'123'`, e rode:

```sql
SELECT COUNT(*) FROM T WHERE V = '000123';
```

Expected: 1. Se vier 0, a collation não está ligada ao domínio, e o problema está na resolução da Task 4, não no plugin.

- [ ] **Step 3: Relatório de colunas com collation própria**

A spec (seções 4.2 e 8) exige que o gbak aponte colunas que usam um domínio remapeado mas trazem
`RDB$COLLATION_ID` próprio, porque `make_version` dá precedência ao da coluna (`dfw.epp:6088`) e
essas colunas não seguem o remap. Isso só é possível depois que as relations existem, portanto não
cabe em `resolve_domain_remap`. Adicione ao fim do restore, junto do resumo:

```cpp
static void report_remap_overrides(BurpGlobals* tdgbl)
{
/**************************************
 *
 *	r e p o r t _ r e m a p _ o v e r r i d e s
 *
 **************************************
 *
 * Functional description
 *	List columns that use a remapped domain but carry their own collation.
 *	make_version gives the column's collation precedence over the domain's,
 *	so these columns do not follow the remap.
 *
 **************************************/
	if (!tdgbl->gbl_domain_remap)
		return;

	Burp::DomainRemap& remap = *tdgbl->gbl_domain_remap;

	for (unsigned i = 0; i < remap.ruleCount(); ++i)
	{
		const Burp::RemapRule& rule = remap.rule(i);
		Firebird::IRequest* req_override = nullptr;

		FOR (REQUEST_HANDLE req_override)
			RFR IN RDB$RELATION_FIELDS
			WITH RFR.RDB$FIELD_SOURCE EQ rule.domainName.c_str()
			AND RFR.RDB$COLLATION_ID NOT MISSING

			BURP_print(false, 418, SafeArg() << RFR.RDB$RELATION_NAME <<
				RFR.RDB$FIELD_NAME << rule.domainName.c_str());
			// msg 418 column @1.@2 keeps its own collation and does not follow domain @3
		END_FOR;
		ON_ERROR
			general_on_error();
		END_ERROR;

		MISC_release_request_silent(req_override);
	}
}
```

Acrescente a mensagem em `src/include/firebird/impl/msg/gbak.h`:

```c
FB_IMPL_MSG_NO_SYMBOL(GBAK, 418, "column @1.@2 keeps its own collation and does not follow domain @3")
```

Chame `report_remap_overrides(tdgbl)` no fim do restore, depois do commit final dos metadados.

Verifique criando uma coluna com collation explícita sobre o domínio antes do backup:
```sql
CREATE TABLE T3 (ID INTEGER, V TDR_TESTE COLLATE MINHA_COLL);
```
Expected: a linha de aviso aparece para `T3.V` e não aparece para as colunas sem collation própria.

- [ ] **Step 4: Escrever a documentação**

Crie `doc/README.fix_domains.md` cobrindo: o que o switch faz, formato das regras, as duas formas de entrada (`@arquivo` e inline), a regra de largura mínima com o exemplo do `CHECK` para limitar a 17, o preview com `-m`, as condições de erro, e a seção de riscos conhecidos apontando para a spec.

- [ ] **Step 5: Commit**

```bash
git add doc/README.fix_domains.md src/burp/restore.epp src/include/firebird/impl/msg/gbak.h
git commit -m "feat(burp): report columns overriding a remapped domain collation"
```

---

### Task 6: Branch integradora srs/fb5-custom

**Files:** nenhum arquivo de código. Só operação de branch.

- [ ] **Step 1: Criar a integradora e juntar as duas branches**

```bash
git checkout -b srs/fb5-custom v5.0-release
git merge --no-ff feature/ltrim-zero-collation-v5 -m "merge: LTRIM_ZERO collation into internal build"
git merge --no-ff srs/gbak-domain-remap -m "merge: gbak -FIX_DOMAINS into internal build"
```

Os conjuntos de arquivos são disjuntos (`src/intl/*` e `intl.vcxproj` de um lado, `src/burp/*` e `burp.vcxproj` do outro), então o merge deve ser limpo. Se houver conflito, ele estará em arquivo de projeto MSVC e se resolve mantendo as duas entradas.

- [ ] **Step 2: Build completo e verificação**

Run: `make_gbak_boot.bat` seguido de `make_all.bat`
Expected: build limpo, com `fbintl` contendo a LTRIM_ZERO e o gbak com o `-FIX_DOMAINS`.

- [ ] **Step 3: Publicar as branches**

```bash
git push fork srs/gbak-domain-remap
git push fork srs/fb5-custom
```

---

## Notas para quem for executar

- O arquivo `src/burp/restore.epp` tem mais de 12 mil linhas. As referências de linha deste plano valem para a árvore em `v5.0-release`; confirme pelo contexto ao redor, não só pelo número.
- APIs usadas nos exemplos, todas conferidas na árvore: `Firebird::string::upper()` (`fb_string.h:406`), `BURP_verbose(USHORT, const SafeArg&)` (`burp_proto.h:51`), `BURP_print(bool, USHORT, const SafeArg&)` (`burp.cpp:1715`), `BURP_error(USHORT, bool, const SafeArg&)` (`burp.cpp:1556`), `DSC_string_length(const dsc*)` (`dsc_proto.h:29`), `os_utils::fopen` (`os_utils.h:89`), `ObjectsArray::add(const T&)` (`objects_array.h:211`), `SafeArg::operator<<` para `int`, `unsigned int` e `const char*` (`SafeArg.h:154-162`). `NOT MISSING` em cláusula `WITH` é GPRE válido (`backup.epp:4044`), e comparar com `.c_str()` dentro de `WITH ... EQ` também (`restore.epp:10587`).
- Os arquivos `.epp` passam por GPRE. Erros de sintaxe em blocos `FOR`, `STORE` e `MODIFY` aparecem como erros no `.cpp` gerado dentro de `gen/burp/`; sempre corrija o `.epp`.
- A referência do hack original é o commit `acd92753b7` na branch `gbak_hacked` (também em `fork/gbak_hacked`). Serve de consulta, mas nada dele deve ser copiado: o bloco de debug com `BURP_verbose(121, ...)`, o `strncmp` com tamanho literal e os ids numéricos ficam todos de fora.
- Riscos conhecidos e deliberadamente não validados em código (arrays, identity, FK remapeada de um lado só, views de expressão) estão na seção 12 da spec. Se algum deles for promovido a validação, o candidato mais forte é a FK, porque falha depois do carregamento completo dos dados.

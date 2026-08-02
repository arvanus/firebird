# Ordenação numérica na collation LTRIM_ZERO - Plano de implementação

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fazer a collation `LTRIM_ZERO` ordenar pelo tamanho da forma normalizada antes do conteúdo, para que `ORDER BY` e `BETWEEN` parem de tratar `0001A34` como menor que `9`, e entregar a receita de build e o runbook de troca em produção.

**Architecture:** Três frentes. (1) `src/intl/lc_ltrim_zero.cpp` passa a comparar tamanho normalizado primeiro e a montar a chave de índice como `[2 bytes: tamanho, big-endian][normalizado em caixa alta]`, devolvendo chave vazia para `INTL_KEY_PARTIAL`. (2) Uma receita MSVC gera o `fbltrimzero.dll` standalone a partir do mesmo fonte, validada antes e depois da mudança de comportamento contra uma instalação estoque do Firebird 5. (3) Um inventário SQL e um runbook cobrem a troca no `SCHERER_001`, onde todo índice existente guarda chave no formato antigo.

**Tech Stack:** C++17, MSVC 2022 (`builds/win32/msvc15`), Boost.Test (`engine_test.exe`, `common_test.exe`), isql, gbak, Firebird 5.

## Global Constraints

- Texto gerado (docs, comentários, mensagens, commits) **nunca** usa travessão (`—`) nem traço médio (`–`). Só hífen simples (`-`).
- Comentários de código em inglês, no estilo do arquivo que está sendo editado.
- Callbacks INTL rodam sem barreira de exceção e em todo comparador e toda chave de índice: **sem alocação dinâmica, sem exceção, sem tocar no memory pool**.
- `texttype_fn_string_to_key` **nunca** pode devolver `INTL_BAD_KEY_LENGTH`. O retorno não é conferido (`intl.cpp:1245-1249`, `btr.cpp:2882`) e `btr.cpp:2926` trunca `(USHORT) -1` para o tamanho máximo, montando chave com memória não inicializada.
- Formato da chave: `[2 bytes tamanho do normalizado, big-endian][bytes normalizados em caixa alta]`. Big-endian porque a ordem byte a byte da chave tem que reproduzir `compare()`.
- `texttype_fn_key_length` devolve `len + 2`.
- Normalização **não muda**: espaços à direita saem quando `PAD SPACE`, zeros e espaços à esquerda saem sempre, comparação em caixa alta ASCII. Igualdade não muda: `'000123' = '123'`.
- Só existem duas collations registradas: `ISO8859_1_LTRIM_ZERO` e `WIN1252_LTRIM_ZERO` (`src/intl/ld.cpp:386,445` e `fbltrimzero.conf`). Não existem variantes UTF8, NONE ou DOS850.
- Ambiente de build, obrigatório antes de qualquer `.bat`:
  ```
  $env:VS170COMNTOOLS='C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\'
  cmd /c 'cd /d D:\GitHub\firebird\builds\win32 & set PATH=.;%PATH% & call <script>.bat'
  ```
- `make_boot.bat` antes de `make_all.bat` sempre que trocar de branch, senão `gen/` velho quebra o build.
- Binários do build saem em `D:\GitHub\firebird\temp\x64\Release\firebird\`.
- **Não copiar o nosso `fbintl.dll` para a instalação de produção**: registraria a mesma collation duas vezes. Em produção só se troca o `fbltrimzero.dll`.
- Branch de trabalho: `srs/ltrim-zero-numeric-order`.

---

## Estrutura de arquivos

**Frente 1 - collation e testes**

| Arquivo | Responsabilidade |
|---|---|
| `src/intl/lc_ltrim_zero.cpp` | Modificar. Driver da collation: normalização, `compare`, `string_to_key`, `key_length`, `canonical`, `init`. |
| `src/jrd/tests/LtrimZeroKeyTest.cpp` | Criar. Teste unitário direto, em processo, sem banco: invariante chave x `compare()`. Compila uma cópia privada do driver. |
| `builds/win32/msvc15/engine_test.vcxproj` | Modificar. Registrar o novo arquivo de teste. |
| `builds/win32/msvc15/engine_test.vcxproj.filters` | Modificar. Mesmo registro, para a árvore do Solution Explorer. |
| `src/jrd/tests/LtrimZeroCollationTest.cpp` | Modificar. Casos de integração novos: ordem numérica, `BETWEEN`, `STARTING WITH` índice x varredura, volume no índice descendente. |
| `test_ltrim_zero.sql` | Modificar. Expectativas de ordenação. |
| `doc/README.ltrim_zero.md`, `doc/ltrim_zero_code_review.md` | Modificar. Documentação do comportamento. |
| `docs/superpowers/specs/2026-08-02-ltrim-zero-numeric-order-design.md` | Modificar. Corrigir a lista de charsets da seção 6 e fechar a decisão da seção 6 sobre `ALTER INDEX`. |

**Frente 2 - módulo standalone**

| Arquivo | Responsabilidade |
|---|---|
| `src/intl/ltrimzero/ld_min.cpp` | Criar. Camada mínima de exportação (`LD_version`, `LD_lookup_texttype_with_status`) que inclui o driver verbatim. Cópia do que hoje vive em `D:\GitHub\ltrim-zero-module`. |
| `src/intl/ltrimzero/fbltrimzero.conf` | Criar. Bloco `intl_module` + `charset` para a instalação de destino. |
| `builds/win32/make_ltrimzero.bat` | Criar. Receita MSVC do `fbltrimzero.dll`. |
| `doc/README.fbltrimzero_build.md` | Criar. Como gerar, conferir e instalar o módulo. |

`src/intl/ltrimzero/` é subdiretório: nem o glob POSIX (`dirFiles` em `builds/posix/make.shared.variables:4`, não recursivo) nem o `intl.vcxproj` (lista explícita) o pegam, então `ld_min.cpp` não entra no `fbintl` e não há símbolo `LD_version` duplicado.

**Frente 3 - rollout**

| Arquivo | Responsabilidade |
|---|---|
| `doc/ltrim_zero_rollout_inventory.sql` | Criar. Inventário: collations do módulo, colunas, segmentos de índice, índices de expressão, índices perto do limite de chave. |
| `doc/README.ltrim_zero_rollout.md` | Criar. Runbook da janela de troca, no formato do `README_collate_tdr_cnpj.md` que já existe no cliente. |

---

## Task 1: Receita MSVC do `fbltrimzero.dll`, validada contra o código atual

Esta task vem primeiro **de propósito**. Gerando e validando o dll com o driver ainda inalterado, uma falha de carga tem uma causa só ("receita errada"). Depois da Task 2 ela teria duas.

**Files:**
- Create: `src/intl/ltrimzero/ld_min.cpp`
- Create: `src/intl/ltrimzero/fbltrimzero.conf`
- Create: `builds/win32/make_ltrimzero.bat`
- Create: `doc/README.fbltrimzero_build.md`

**Interfaces:**
- Consumes: `src/intl/lc_ltrim_zero.cpp` (entry point `LCLTRIMZERO_init`, assinatura do `TEXTTYPE_ENTRY3`), `src/intl/ld_proto.h` (protótipos `extern "C"` de `LD_version` e `LD_lookup_texttype_with_status`).
- Produces: `fbltrimzero.dll` x64 e o script `builds/win32/make_ltrimzero.bat`. A Task 5 roda o mesmo script sem alteração.

- [ ] **Step 1: Copiar a camada de exportação para dentro do repositório**

Criar `src/intl/ltrimzero/ld_min.cpp` com exatamente este conteúdo (é o arquivo de `D:\GitHub\ltrim-zero-module\ld_min.cpp`, sem mudança de código, só trazido para o repositório para que driver e camada de exportação nunca divirjam):

```cpp
/*
 * Minimal INTL module exposing only the LTRIM_ZERO collation.
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
 * Build: builds/win32/make_ltrimzero.bat on Windows, or the Makefile in
 * doc/README.fbltrimzero_build.md on Linux.
 */

#include "firebird.h"
#include "intl/ldcommon.h"
#include "intl/ld_proto.h"

#include <string.h>

// ld_proto.h declares this; ld.cpp defines it in the full module.
USHORT version = INTL_VERSION_2;

// The collation driver, compiled straight into this module.
#include "intl/lc_ltrim_zero.cpp"

namespace
{
	// Names must match the collation names used in fbltrimzero.conf.
	const char* const COLLATIONS[] =
	{
		"WIN1252_LTRIM_ZERO",
		"ISO8859_1_LTRIM_ZERO",
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
			"fbltrimzero: this module only provides the LTRIM_ZERO collations");
		return false;
	}

	// The driver uses TEXTTYPE_ENTRY3, so the charset argument is unused and
	// passing NULL is safe. That is what keeps this module independent from
	// fbintl: it never has to build a charset of its own.
	const INTL_BOOL ok = LCLTRIMZERO_init(tt, nullptr, texttype_name, charset_name,
		attributes, specific_attributes, specific_attributes_length, nullptr);

	if (!ok)
	{
		report(status_buffer, status_buffer_length,
			"fbltrimzero: unsupported attributes. Only CASE INSENSITIVE and"
			" PAD SPACE are accepted, and specific attributes are not supported");
	}

	return ok;
}
```

- [ ] **Step 2: Copiar o arquivo de configuração**

Criar `src/intl/ltrimzero/fbltrimzero.conf` com o conteúdo de `D:\GitHub\ltrim-zero-module\fbltrimzero.conf`, sem alteração:

```
# Firebird INTL module: LTRIM_ZERO collation.
#
# Drop this file and fbltrimzero.dll into the intl directory of the target
# installation, next to fbintl.conf and fbintl.dll. Do NOT edit fbintl.conf:
# the engine scans every *.conf in that directory
# (Jrd::IntlManager::initialize -> ScanDir(intlPath, "*.conf")).
#
# Registration is per charset:collation pair and the charset itself is
# resolved separately, so declaring a charset block here that names a
# different intl_module does not conflict with the one fbintl declares. The
# WIN1252 and ISO8859_1 charsets keep coming from fbintl; only these two
# collations come from this module.
#
# After copying, restart the server, then in each database:
#
#   CREATE COLLATION WIN1252_LTRIM_ZERO FOR WIN1252
#       FROM EXTERNAL ('WIN1252_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE;
#
# Both clauses matter. PAD SPACE is what makes CHAR columns work. CASE
# INSENSITIVE is what keeps SIMILAR TO consistent with LIKE and CONTAINING,
# because SIMILAR TO compiles to RE2 and reads the flag from the collation
# attributes instead of going through the canonical form.

intl_module = fbltrimzero {
    filename = $(this)/fbltrimzero
}

charset = WIN1252 {
    intl_module = fbltrimzero
    collation = WIN1252_LTRIM_ZERO
}

charset = ISO8859_1 {
    intl_module = fbltrimzero
    collation = ISO8859_1_LTRIM_ZERO
}
```

- [ ] **Step 3: Escrever a receita MSVC**

Criar `builds/win32/make_ltrimzero.bat`:

```bat
@echo off
::
:: Builds fbltrimzero.dll, a standalone Firebird INTL module carrying only the
:: LTRIM_ZERO collations, out of src/intl/ltrimzero/ld_min.cpp.
::
:: The module has no run time dependency on the Firebird libraries: the driver
:: allocates nothing, throws nothing and does not use ICU. It is linked against
:: the static CRT (/MT) on purpose, so that dropping it into a customer server
:: never drags in a Visual C++ redistributable. Nothing crosses the CRT
:: boundary: the engine owns every buffer the entry points touch.
::
:: Compile flags mirror the Release x64 settings of intl.vcxproj and
:: FirebirdCommon.props, except for /MT (props use /MD) and for INTL_EXPORTS,
:: which fbintl uses only for its own resource script.
::
:: Usage, from builds\win32, after setenvvar.bat:
::     make_ltrimzero.bat
::
:: Output: builds\win32\ltrimzero\fbltrimzero.dll
::
@echo on

@if "%FB_ROOT_PATH%"=="" (
    @echo Run setenvvar.bat first.
    @exit /b 1
)

@set LTZ_OUT=%~dp0ltrimzero
@if not exist "%LTZ_OUT%" mkdir "%LTZ_OUT%"

cl /nologo ^
   /O2 /MT /GR- /std:c++17 /W3 ^
   /D NDEBUG /D _WINDOWS /D _USRDLL /D WINDOWS_ONLY /D SUPERCLIENT ^
   /D WIN32 /D _CRT_SECURE_NO_WARNINGS ^
   /I "%FB_ROOT_PATH%\src" ^
   /I "%FB_ROOT_PATH%\src\include" ^
   /I "%FB_ROOT_PATH%\src\include\gen" ^
   /I "%FB_ROOT_PATH%\src\jrd" ^
   /LD "%FB_ROOT_PATH%\src\intl\ltrimzero\ld_min.cpp" ^
   /Fo"%LTZ_OUT%\\" /Fe"%LTZ_OUT%\fbltrimzero.dll" ^
   /link /OPT:REF /OPT:ICF

@if errorlevel 1 (
    @echo BUILD FAILED
    @exit /b 1
)

@copy /Y "%FB_ROOT_PATH%\src\intl\ltrimzero\fbltrimzero.conf" "%LTZ_OUT%\" >nul

@echo.
@echo --- exported entry points
dumpbin /nologo /exports "%LTZ_OUT%\fbltrimzero.dll" | findstr /C:"LD_version" /C:"LD_lookup_texttype_with_status"
@echo.
@echo --- dependencies
dumpbin /nologo /dependents "%LTZ_OUT%\fbltrimzero.dll"
```

`FB_ROOT_PATH` é definida por `setenvvar.bat`. Os nomes exportados saem sem decoração porque `ld_proto.h` embrulha os protótipos em `extern "C"` e `FB_DLL_EXPORT` é `__declspec(dllexport)` no Windows (`src/include/firebird/ibase.h:67-68`); não é preciso arquivo `.def`.

- [ ] **Step 4: Compilar e conferir os exports**

Rodar:

```
$env:VS170COMNTOOLS='C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\'
cmd /c 'cd /d D:\GitHub\firebird\builds\win32 & set PATH=.;%PATH% & call setenvvar.bat & call make_ltrimzero.bat'
```

Esperado:
- `builds\win32\ltrimzero\fbltrimzero.dll` existe;
- `dumpbin /exports` lista `LD_version` e `LD_lookup_texttype_with_status` **sem decoração** (nada de `?LD_version@@YAXPEAG@Z`);
- `dumpbin /dependents` lista só `KERNEL32.dll`. Se aparecer `VCRUNTIME140.dll` ou `MSVCP140.dll`, o `/MT` não pegou: corrigir antes de seguir.

Se o compilador reclamar de `autoconfig.h` faltando, conferir que `/I src\include\gen` está na linha: no Windows o arquivo usado é `src\include\gen\autoconfig_msvc.h`, que já está versionado, e não é preciso rodar `configure`.

- [ ] **Step 5: Capturar a linha de base com o dll que já está em produção**

Antes de trocar qualquer coisa, rodar a suíte SQL contra o `fbltrimzero.dll` **atualmente instalado**:

```
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" -u SYSDBA -p masterkey ^
  -i D:\GitHub\firebird\test_ltrim_zero.sql -o D:\GitHub\firebird\ltz_stock_baseline.txt
```

Isso existe porque o dll instalado foi gerado há tempos, a partir de uma versão antiga do fonte, sem receita guardada. Sem essa captura, uma diferença no passo 7 seria diagnosticada como receita quebrada quando na verdade é o fonte que andou.

- [ ] **Step 6: Instalar na instalação estoque**

Ainda com o driver **inalterado**, para provar a receita e não o código novo.

```powershell
# 1. Parar o serviço
Stop-Service FirebirdServerDefaultInstance

# 2. Guardar o dll antigo
$intl = 'C:\Program Files\Firebird\Firebird_5_0\intl'
Copy-Item "$intl\fbltrimzero.dll" "$intl\fbltrimzero.dll.bak_pre_numeric" -Force

# 3. Instalar o novo
Copy-Item 'D:\GitHub\firebird\builds\win32\ltrimzero\fbltrimzero.dll' $intl -Force

# 4. Subir
Start-Service FirebirdServerDefaultInstance
```

O `fbintl.conf` instalado já traz `#include $(root)/intl/fbltrimzero.conf`, então o `.conf` não precisa ser recopiado. Conferir com `Select-String -Path "$intl\fbintl.conf" -Pattern fbltrimzero`.

- [ ] **Step 7: Rodar a suíte SQL contra o dll recém-gerado e comparar**

```
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" -u SYSDBA -p masterkey ^
  -i D:\GitHub\firebird\test_ltrim_zero.sql -o D:\GitHub\firebird\ltz_stock_recipe.txt
```

```powershell
Compare-Object (Get-Content D:\GitHub\firebird\ltz_stock_baseline.txt) `
               (Get-Content D:\GitHub\firebird\ltz_stock_recipe.txt)
```

Como ler o resultado, nessa ordem:

1. **O dll carregou e `CREATE COLLATION ... FROM EXTERNAL` resolveu?** Se sim, a receita está provada. É esse o objetivo da task. Se não (erro de carga, collation não encontrada, export faltando), o problema é a receita: restaurar `fbltrimzero.dll.bak_pre_numeric`, subir o serviço e corrigir o `.bat` antes de seguir.
2. **Sobrou alguma diferença de comportamento?** Então o fonte deste repositório andou em relação ao binário instalado. Registrar quais casos diferem e seguir: operacionalmente é inofensivo, porque o rollout da Task 6 é backup e restore, que reconstrói todo índice a partir do driver novo de qualquer jeito.

O teste `9.1 KNOWN LIMIT: ordering is lexicographic, not numeric` ainda tem que sair com `10,100,9,a,B` nos dois relatórios: o comportamento de ordenação só muda na Task 2.

- [ ] **Step 8: Documentar a receita**

Criar `doc/README.fbltrimzero_build.md` cobrindo, em prosa curta:
- o que é o módulo e por que ele existe separado do `fbintl` (a produção roda engine estoque; trocar o `fbintl.dll` registraria a collation duas vezes);
- a receita Windows: `setenvvar.bat` + `make_ltrimzero.bat`, e onde sai o binário;
- a decisão do `/MT` e por que é segura (nenhum objeto de CRT atravessa a fronteira do módulo: o engine é dono de todo buffer que os entry points tocam);
- como conferir: `dumpbin /exports` (dois nomes, sem decoração) e `dumpbin /dependents` (só `KERNEL32.dll`);
- a receita Linux equivalente, transcrevendo o `Makefile` de `D:\GitHub\ltrim-zero-module` com `FB_SRC` apontando para a árvore configurada, e os checks `nm -D`, `ldd`, `objdump -T`;
- instalação: copiar `fbltrimzero.dll` e `fbltrimzero.conf` para o diretório `intl` do destino, **sem editar** `fbintl.conf`, e reiniciar o serviço.

- [ ] **Step 9: Commit**

```bash
git add src/intl/ltrimzero builds/win32/make_ltrimzero.bat doc/README.fbltrimzero_build.md
git commit -m "build(intl): MSVC recipe for the standalone fbltrimzero module"
```

---

## Task 2: Ordem numérica no driver, com teste unitário direto de chave

**Files:**
- Create: `src/jrd/tests/LtrimZeroKeyTest.cpp`
- Modify: `builds/win32/msvc15/engine_test.vcxproj` (bloco `ItemGroup` de `ClCompile`, hoje nas linhas 183-192)
- Modify: `builds/win32/msvc15/engine_test.vcxproj.filters`
- Modify: `src/intl/lc_ltrim_zero.cpp` (cabeçalho de comentário linhas 36-57, `texttype_fn_compare` linhas 103-142, `texttype_fn_str_to_key` linhas 145-184, `texttype_fn_key_length` linhas 187-191)

**Interfaces:**
- Consumes: `LCLTRIMZERO_init(texttype* cache, charset* cs, const ASCII* tt_name, const ASCII* cs_name, USHORT attributes, const UCHAR* specific_attributes, ULONG specific_attributes_length, const ASCII* config_info)` retornando `INTL_BOOL`; ponteiros `texttype::texttype_fn_compare`, `texttype::texttype_fn_string_to_key`, `texttype::texttype_fn_key_length`; constantes `INTL_KEY_SORT` (0), `INTL_KEY_PARTIAL` (1), `INTL_KEY_UNIQUE` (2) de `src/common/intlobj_new.h:60-62`; `TEXTTYPE_ATTR_PAD_SPACE` (1), `TEXTTYPE_ATTR_CASE_INSENSITIVE` (2).
- Produces: o formato de chave `[2 bytes big-endian][normalizado]`, `key_length(len) == len + 2` e `string_to_key(..., INTL_KEY_PARTIAL) == 0`. A Task 3 e o runbook da Task 6 dependem desses três fatos.

- [ ] **Step 1: Escrever o teste que falha**

Criar `src/jrd/tests/LtrimZeroKeyTest.cpp`:

```cpp
/*
 *	PROGRAM:	JRD engine tests
 *	MODULE:		LtrimZeroKeyTest.cpp
 *	DESCRIPTION:	Unit tests for the LTRIM_ZERO ordering and sort key
 *
 * These tests call the collation driver directly, in process, with no server
 * and no database. They own the one invariant everything else rests on: the
 * sign of compare() must equal the sign of a plain byte comparison of the two
 * sort keys. If those ever disagree, an index stops describing the order the
 * engine believes it describes, and UNIQUE, DISTINCT and BETWEEN all start
 * lying. LtrimZeroCollationTest.cpp checks the same property through a real
 * b-tree, which is slower and much harder to read when it breaks.
 *
 * Run only this suite with:
 *     engine_test --run_test=IntlSuite/LtrimZeroKeySuite
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
// that lives inside fbintl. src/intl/ltrimzero/ld_min.cpp uses the same trick.
#define LCLTRIMZERO_init LCLTRIMZERO_init_under_test
#include "../../intl/lc_ltrim_zero.cpp"
#undef LCLTRIMZERO_init

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

		const INTL_BOOL ok = LCLTRIMZERO_init_under_test(&tt, nullptr,
			"WIN1252_LTRIM_ZERO", "WIN1252", attributes, nullptr, 0, nullptr);

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
BOOST_AUTO_TEST_SUITE(LtrimZeroKeySuite)


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


BOOST_AUTO_TEST_SUITE_END()	// LtrimZeroKeySuite
BOOST_AUTO_TEST_SUITE_END()	// IntlSuite
```

- [ ] **Step 2: Registrar o teste no projeto MSVC**

Em `builds/win32/msvc15/engine_test.vcxproj`, no `ItemGroup` de `ClCompile` (hoje linhas 183-192), acrescentar depois da linha do `LtrimZeroCollationTest.cpp`:

```xml
    <ClCompile Include="..\..\..\src\jrd\tests\LtrimZeroKeyTest.cpp" />
```

Em `builds/win32/msvc15/engine_test.vcxproj.filters`, acrescentar a entrada correspondente, copiando o filtro usado pelo `LtrimZeroCollationTest.cpp` no mesmo arquivo.

No POSIX não é preciso mexer em nada: `Engine_Test_Objects:= $(call dirObjects,jrd/tests)` (`builds/posix/make.shared.variables:93`) varre o diretório.

- [ ] **Step 3: Compilar e rodar o teste, esperando falha**

```
$env:VS170COMNTOOLS='C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\'
cmd /c 'cd /d D:\GitHub\firebird\builds\win32 & set PATH=.;%PATH% & call make_boot.bat & call make_all.bat'
D:\GitHub\firebird\temp\x64\Release\firebird\engine_test.exe --run_test=IntlSuite/LtrimZeroKeySuite --log_level=test_suite
```

Esperado: **FAIL**. Com o driver atual:
- `ShorterNormalizedFormSortsFirst`: `compare("9","0001A34")` devolve `1`, esperado `-1`;
- `ClassesAreTotallyOrdered`: `'A'` antes de `'10'` falha, porque hoje `'A' = 0x41 > '1' = 0x31`;
- `KeyFormatIsLengthPrefixed`: chave de `0001A34` tem 4 bytes, esperado 6;
- `KeyLengthAccountsForThePrefix`: `key_length(20)` devolve 20, esperado 22;
- `PartialKeyIsEmpty`: chave parcial de `9` tem 1 byte, esperado 0;
- `RangeOverDifferentLengthsExcludesShorterValues`: `compare("12345678901", "12345678000000")` devolve `1`, esperado `-1`. É exatamente o `BETWEEN` errado da seção 1 da spec.

`KeyOrderReproducesCompare` **passa** antes da mudança, porque a invariante chave x `compare` já vale hoje. Isso é esperado: esse caso é a rede de segurança da refatoração, não a especificação da mudança. Se ele falhar depois do passo 8, chave e comparação divergiram.

- [ ] **Step 4: Implementar a comparação por tamanho**

Em `src/intl/lc_ltrim_zero.cpp`, substituir o corpo de `texttype_fn_compare` a partir do laço (linhas 124-141) por:

```cpp
	// Numeric ordering: the shorter normalized form always sorts first,
	// whatever its bytes are. This is what puts '9' before '0001A34' and what
	// keeps BETWEEN over a range of same length values from swallowing a
	// shorter one. The keys built below reproduce this by carrying the length
	// in front.
	const ULONG n1 = (ULONG) (e1 - p1);
	const ULONG n2 = (ULONG) (e2 - p2);

	if (n1 != n2)
		return (n1 < n2) ? -1 : 1;

	while (p1 < e1)
	{
		const UCHAR c1 = ascii_toupper(*p1++);
		const UCHAR c2 = ascii_toupper(*p2++);

		if (c1 != c2)
			return (c1 < c2) ? -1 : 1;
	}

	return 0;
```

- [ ] **Step 5: Implementar a chave prefixada pelo tamanho**

Substituir `texttype_fn_str_to_key` (linhas 145-184) inteiro por:

```cpp
static USHORT texttype_fn_str_to_key(texttype* obj,
									 USHORT srcLen, const UCHAR* src,
									 USHORT dstLen, UCHAR* dst,
									 USHORT key_type)
{
	fb_assert(src != NULL || srcLen == 0);
	fb_assert(dst != NULL);

	// A partial key cannot exist in this format. The key starts with the
	// length of the WHOLE normalized string, so the key of a prefix is not a
	// byte prefix of the key of the value, and no amount of padding makes it
	// one. An empty key is the safe answer: BTR_make_key flags it as empty
	// (btr.cpp:1900), a fuzzy scan with an empty key does not stop early
	// (btr.cpp:1904-1908, 6893, 6939) and turns into a full index scan, and
	// blr_starting is re-evaluated over the record afterwards
	// (Optimizer.cpp:3008-3035), so STARTING WITH stays correct and only gets
	// slower.
	//
	// INTL_BAD_KEY_LENGTH must NOT be used for this. The return value is not
	// checked (intl.cpp:1245-1249, btr.cpp:2882) and btr.cpp:2926 truncates
	// (USHORT) -1 into the maximum key size, building a key out of
	// uninitialized buffer memory.
	if (key_type == INTL_KEY_PARTIAL)
		return 0;

	const UCHAR* p;
	const UCHAR* end;
	normalize_bounds(obj->texttype_pad_option != 0, src, srcLen, p, end);

	const USHORT len = (USHORT) (end - p);

	// [2 bytes: length of the normalized form, big endian][normalized bytes]
	//
	// Big endian so that a plain byte comparison of two keys reproduces
	// compare(): length first, content second. INTL_KEY_UNIQUE is identical to
	// INTL_KEY_SORT, because this key already defines the equality class.
	if (dstLen < 2)
	{
		fb_assert(false);	// key_length() promised at least len + 2
		return 0;
	}

	dst[0] = (UCHAR) (len >> 8);
	dst[1] = (UCHAR) (len & 0xFF);

	USHORT pos = 2;

	while (p < end && pos < dstLen)
		dst[pos++] = ascii_toupper(*p++);

	// dstLen can legitimately be smaller than len + 2, so the loop above must
	// stop on it and there is nothing to assert here. INTL_key_length caps the
	// key at MAX_KEY = 8192 and then raises it back to the raw field length
	// (intl.cpp:1013-1017), so a VARCHAR(12000) of non strippable characters
	// gets dstLen = 12000 for a key that would want 12002. That truncation
	// predates this change and is not checked by the caller
	// (SortedStream.cpp:265). The length prefix stays truthful, so two values
	// of different normalized length still get different keys even when both
	// bodies are cut at the same point, which is strictly better than the old
	// format, where truncation lost the distinction entirely.

	// Do not pad the rest of dst. On the index path the engine passes
	// dstLen = 32767 no matter how long the value is, and it uses only the
	// returned length. Padding would memset 32 KB per key.

	return pos;
}
```

- [ ] **Step 6: Ajustar o comprimento declarado da chave**

Substituir `texttype_fn_key_length` (linhas 187-191) por:

```cpp
static USHORT texttype_fn_key_length(texttype* /*obj*/, USHORT len)
{
	// Normalization only removes characters, so the normalized part never
	// exceeds len. The two extra bytes are the big endian length prefix
	// written by texttype_fn_str_to_key. len comes from a column width, capped
	// by MAX_COLUMN_SIZE (32767), so len + 2 cannot wrap a USHORT.
	return len + 2;
}
```

- [ ] **Step 7: Reescrever o cabeçalho de comentário do arquivo**

O bloco atual (linhas 36-57) afirma duas coisas que passam a ser falsas: "The sort key IS the normalized string" e "INTL_KEY_PARTIAL needs no special case either". Substituir os dois primeiros bullets de "Implementation notes" por:

```
 *  - compare() and string_to_key() share the same normalization primitive
 *    (normalize_bounds) on purpose, and both order by the LENGTH of the
 *    normalized form before its bytes. The sort key is that length as two
 *    big endian bytes followed by the normalized string in upper case, so a
 *    byte comparison of two keys reproduces compare() exactly, which is what
 *    the b-tree relies on. Keep them in sync: if they ever diverge, UNIQUE
 *    constraints and DISTINCT stop agreeing with '='. For the same reason
 *    TEXTTYPE_SEPARATE_UNIQUE is not needed.
 *
 *  - Ordering by length first is what makes the collation behave numerically
 *    for values that are digit strings of different widths: '9' sorts before
 *    '0001A34', and BETWEEN over a range of 14 digit values does not swallow
 *    an 11 digit one. Equality is unchanged: '000123' still equals '123'.
 *
 *  - INTL_KEY_PARTIAL returns an empty key, because with the length in front
 *    the key of a prefix is not a prefix of the key of the value. The engine
 *    turns an empty starting key into a full index scan and re-checks
 *    blr_starting against the record, so STARTING WITH and LIKE 'x%' stay
 *    correct and only lose the index range. See the comment inside
 *    texttype_fn_str_to_key for the exact chain.
```

E, no bullet sobre pattern matching (linhas 51-57), trocar a frase final "As a consequence STARTING WITH may return different rows depending on whether an index is used" por: "STARTING WITH is still evaluated over the record after the index scan, so both plans return the same rows; what changes is that the index no longer narrows the range."

- [ ] **Step 8: Recompilar e rodar o teste, esperando sucesso**

```
cmd /c 'cd /d D:\GitHub\firebird\builds\win32 & set PATH=.;%PATH% & call make_all.bat'
D:\GitHub\firebird\temp\x64\Release\firebird\engine_test.exe --run_test=IntlSuite/LtrimZeroKeySuite --log_level=test_suite
```

Esperado: `*** No errors detected`.

- [ ] **Step 9: Commit**

```bash
git add src/intl/lc_ltrim_zero.cpp src/jrd/tests/LtrimZeroKeyTest.cpp builds/win32/msvc15/engine_test.vcxproj builds/win32/msvc15/engine_test.vcxproj.filters
git commit -m "feat(intl): order LTRIM_ZERO by normalized length before bytes"
```

---

## Task 3: Suíte de engine - ordem numérica, STARTING WITH e volume no índice descendente

**Files:**
- Modify: `src/jrd/tests/LtrimZeroCollationTest.cpp` (acrescentar linhas ao caso `CompoundIndex`, hoje linhas 789-822, e casos novos ao final, antes de `BOOST_AUTO_TEST_SUITE_END()` na linha 1065)

**Interfaces:**
- Consumes: `TestDb` (métodos `ddl`, `exec`, `commit`, `ids`, `one`, `refresh`), `checkSamePlanResult(TestDb&, const char* label, const std::string& query, const std::string& naturalPlan, const std::string& indexPlan)`, `withPlan(const std::string&, const std::string&)`, macro `LTZ_TEST_CASE(name)`, domínio `D_LTZ` (`VARCHAR(20) CHARACTER SET WIN1252 COLLATE LTZ`) criado pelo construtor do `TestDb`.
- Produces: nada consumido por outras tasks.

- [ ] **Step 1: Variar o tamanho normalizado dentro do caso composto**

O caso `CompoundIndex` que já existe só usa valores que normalizam para tamanho 0 ou 1, então o prefixo de tamanho nunca varia entre segmentos e a interação com os stuff bytes não é exercida. Acrescentar duas linhas aos `INSERT` do caso (hoje linhas 795-802), antes do `db.commit()`:

```cpp
	db.exec("INSERT INTO C1 VALUES (9, '0AB',  '12345678901')");
	db.exec("INSERT INTO C1 VALUES (10, 'AB',  '00012345678901')");
```

E acrescentar, depois do último `checkSamePlanResult` do caso:

```cpp
	checkSamePlanResult(db, "compound, segments of different normalized lengths",
		"SELECT CAST(ID AS BIGINT) FROM C1 WHERE A = 'AB' AND B = '12345678901' ORDER BY ID",
		"SORT ((C1 NATURAL))", "SORT ((C1 INDEX (IX_C1)))");

	// Both rows are the same pair of equivalence classes, so both must come
	// back through either plan.
	BOOST_CHECK_EQUAL(db.one(
		"SELECT CAST(COUNT(*) AS BIGINT) FROM C1 WHERE A = 'ab' AND B = '12345678901'"), 2);
```

- [ ] **Step 2: Escrever os casos novos**

Acrescentar em `src/jrd/tests/LtrimZeroCollationTest.cpp`, antes de `BOOST_AUTO_TEST_SUITE_END()	// LtrimZeroSuite`:

```cpp
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

	for (int i = 0; i < 2000; i++)
	{
		char sql[256];
		snprintf(sql, sizeof(sql),
			"INSERT INTO S1 VALUES (%d,"
			" SUBSTRING('0000' FROM 1 FOR MOD(%d, 5) + 1) || 'K' || MOD(%d, 23))",
			i, i, i);
		db.exec(sql);
	}

	db.exec("INSERT INTO S1 VALUES (9001, '000')");
	db.exec("INSERT INTO S1 VALUES (9002, '   ')");
	db.commit();

	db.ddl("CREATE INDEX IX_S1_V ON S1(V)");

	const char* const prefixes[] =
	{
		"K",		// matches many rows
		"K1",		// matches a subset
		"00",		// fully strippable prefix
		"0000",		// fully strippable, longer
		"ZZZ"		// matches nothing
	};

	for (const char* prefix : prefixes)
	{
		const std::string query =
			std::string("SELECT CAST(ID AS BIGINT) FROM S1 WHERE V STARTING WITH '")
			+ prefix + "' ORDER BY ID";

		checkSamePlanResult(db, (std::string("STARTING WITH '") + prefix + "'").c_str(),
			query, "SORT ((S1 NATURAL))", "SORT ((S1 INDEX (IX_S1_V)))");
	}

	// LIKE 'x%' is rewritten into blr_starting and must agree as well.
	checkSamePlanResult(db, "LIKE 'K1%'",
		"SELECT CAST(ID AS BIGINT) FROM S1 WHERE V LIKE 'K1%' ORDER BY ID",
		"SORT ((S1 NATURAL))", "SORT ((S1 INDEX (IX_S1_V)))");

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
	{
		IdList ascending = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM DV"
			" PLAN SORT (DV NATURAL) ORDER BY V, ID DESC");
		const IdList descending = db.ids(
			"SELECT CAST(ID AS BIGINT) FROM DV"
			" PLAN (DV ORDER IX_DV) ORDER BY V DESC, ID");

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
```

- [ ] **Step 3: Rodar a suíte, esperando sucesso**

```
cmd /c 'cd /d D:\GitHub\firebird\builds\win32 & set PATH=.;%PATH% & call make_all.bat'
D:\GitHub\firebird\temp\x64\Release\firebird\engine_test.exe --run_test=EngineSuite/LtrimZeroSuite --log_level=test_suite
```

Esperado: `*** No errors detected`, com os casos antigos (`ConcurrentVolumeAndValidation`, `DescendingIndexAndEmptyKey`, `CompoundIndex`, `PrimaryAndForeignKey`, `BackupRestoreRoundTrip`, `BlobAndTransliteration`) ainda passando: nenhum deles depende da ordem entre tamanhos diferentes.

Se `CollationIsInstalled` falhar, o problema é a árvore de runtime e não o código; conferir que `temp\x64\Release\firebird\intl\fbintl.dll` e `fbintl.conf` existem.

- [ ] **Step 4: Commit**

```bash
git add src/jrd/tests/LtrimZeroCollationTest.cpp
git commit -m "test(intl): cover numeric order, STARTING WITH and descending volume"
```

---

## Task 4: `test_ltrim_zero.sql` e documentação

**Files:**
- Modify: `test_ltrim_zero.sql` (caso `9.1`, linhas 528-532)
- Modify: `doc/README.ltrim_zero.md`
- Modify: `doc/ltrim_zero_code_review.md`
- Modify: `docs/superpowers/specs/2026-08-02-ltrim-zero-numeric-order-design.md` (seções 6 e 6 final)

**Interfaces:**
- Consumes: o formato de chave e a ordem definidos na Task 2.
- Produces: `test_ltrim_zero.sql` verde contra o driver novo, usado como critério de aceite nas Tasks 1 e 5.

- [ ] **Step 1: Trocar a expectativa de ordenação no script SQL**

O único caso do script que muda é o `9.1`. A tabela `T_ORD` tem `'0009'`, `'0010'`, `'00100'`, `'a'`, `'B'`, que normalizam para `9`, `10`, `100`, `a`, `B`. Com a ordem por tamanho: `9`, `a`, `B` (tamanho 1, e `'9'=0x39 < 'A'=0x41 < 'B'=0x42`), depois `10` (tamanho 2), depois `100` (tamanho 3).

Substituir o bloco das linhas 528-532 por:

```sql
INSERT INTO TST (NAME, EXPECTED, ACTUAL)
SELECT '9.1 ordering is numeric: shorter normalized values come first',
       '9,a,B,10,100',
       (SELECT LIST(TRIM(LEADING '0' FROM V), ',') FROM (SELECT V FROM T_ORD ORDER BY V))
FROM RDB$DATABASE;
```

Repare que o `KIND` deixa de ser `'I'`: o caso era informativo porque documentava um limite conhecido, e agora é uma asserção normal. Ajustar a lista de colunas do `INSERT` de acordo, como no bloco acima.

Acrescentar logo abaixo, no mesmo bloco 9, o caso do intervalo que motivou a mudança:

```sql
INSERT INTO TST (NAME, EXPECTED, ACTUAL)
SELECT '9.2 a range over 14 digit values does not swallow an 11 digit one', '2',
       CAST((SELECT COUNT(*) FROM T_RANGE
             WHERE V BETWEEN '12345678000000' AND '12345678999999') AS VARCHAR(80))
FROM RDB$DATABASE;
```

precedido da tabela que ele usa, ainda no bloco 9:

```sql
CREATE TABLE T_RANGE (
    V VARCHAR(20) CHARACTER SET WIN1252 COLLATE WIN1252_LTRIM_ZERO
);

INSERT INTO T_RANGE VALUES ('12345678000199');
INSERT INTO T_RANGE VALUES ('12345678009999');
INSERT INTO T_RANGE VALUES ('12345678901');
COMMIT;
```

- [ ] **Step 2: Reenquadrar o bloco 6b, que perde 2 bytes de cauda**

Este é o único outro caso do script que muda, e muda para pior. Conferir empiricamente antes de editar, rodando só o bloco 6b, porque a conclusão abaixo vem de leitura de código e não de execução.

O raciocínio: `INTL_key_length` (`intl.cpp:1002-1019`) calcula `key_length(12000)`, que agora devolve 12002, corta em `MAX_KEY = 8192` e depois **eleva de volta** para `iLength`, ou seja 12000. O driver recebe então `dstLen = 12000` para uma chave que queria 12002, escreve 2 bytes de prefixo mais 11998 de corpo, e o byte que distingue os dois valores do teste fica no índice 11999, cortado. A coluna `P`, de collation padrão, não passa por `key_length` (`intl.cpp:1003-1004`) e continua com 12000 bytes crus, que distinguem.

Resultado esperado: `6b.1` (GROUP BY) e `6b.3` (DISTINCT) passam a devolver 1 para `V` contra 2 para `P`, e `6b.2` (ORDER BY) passa a devolver uma ordem arbitrária entre os dois, porque as chaves ficam idênticas.

Isso não é regressão nova de correção, é a truncagem de `MAX_KEY` que já existia ficando 2 bytes mais apertada, exatamente como a seção 4.1 da spec registra. Mas o bloco 6b foi escrito como teste discriminante ("uma diferença entre V e P seria regressão deste driver"), e agora existe diferença, então o bloco precisa dizer a verdade nova em vez de falhar.

Reescrever o comentário de cabeçalho do bloco 6b e os três casos, marcando-os como informativos (`KIND = 'I'`, como o `9.1` era) e trocando as expectativas:

```sql
/* ------------------------------------------------------------------ */
/* 6b. Values whose NORMALIZED form is longer than MAX_KEY (8192)      */
/*     The engine caps the sort key at the raw field length            */
/*     (intl.cpp:1002-1019) and does not check the return of           */
/*     string_to_key (SortedStream.cpp:265), so a value wider than     */
/*     MAX_KEY has always had its key truncated. The length prefix     */
/*     this collation writes costs 2 more bytes of tail, so two values */
/*     that differ only in their last 2 bytes now collapse into one    */
/*     key where the default collation still tells them apart.         */
/*     Recorded here, not asserted as equality with the default        */
/*     collation: it only reaches columns wider than 8192 bytes.       */
/* ------------------------------------------------------------------ */
```

```sql
INSERT INTO TST (KIND, NAME, EXPECTED, ACTUAL)
SELECT 'I', '6b.1 KNOWN LIMIT: over MAX_KEY the last 2 bytes no longer separate keys',
       '1',
       CAST((SELECT COUNT(*) FROM (SELECT V FROM T_BIGKEY GROUP BY V)) AS VARCHAR(80))
FROM RDB$DATABASE;

INSERT INTO TST (NAME, EXPECTED, ACTUAL)
SELECT '6b.1b the default collation still separates them, as the control',
       '2',
       CAST((SELECT COUNT(*) FROM (SELECT P FROM T_BIGKEY GROUP BY P)) AS VARCHAR(80))
FROM RDB$DATABASE;
```

Aplicar o mesmo tratamento a `6b.3` (DISTINCT). Para `6b.2`, trocar a comparação de ordem por uma asserção de que a truncagem se dá **na cauda e não no prefixo**, que é o que sustenta a seção 4.1 da spec:

```sql
INSERT INTO TST (NAME, EXPECTED, ACTUAL)
SELECT '6b.2 values of different normalized length still sort apart over MAX_KEY', '2',
       CAST((SELECT COUNT(*) FROM (
              SELECT V FROM T_BIGKEY
              UNION
              SELECT LPAD(_WIN1252 'A', 11000, _WIN1252 'X') FROM RDB$DATABASE
            )) AS VARCHAR(80))
FROM RDB$DATABASE;
```

Se a execução do bloco 6b mostrar comportamento diferente do descrito, **parar e reconciliar** antes de editar: a leitura de `intl.cpp` acima é a única base dessa previsão.

- [ ] **Step 3: Conferir que os demais casos do script continuam válidos**

Não mexer em nada, só confirmar por leitura antes de rodar:
- `6.5` espera `1,3,2`. Os três valores normalizam para `A`, `B` e `A`, todos de tamanho 1, então a ordem não muda.
- `8.6`, `8.6b`, `8.6c` e `8.7` comparam `STARTING WITH` entre plano natural e plano de índice. A chave parcial vazia vira varredura completa com filtro residual, então as contagens continuam iguais.
- `4.6` compara dois planos entre si; é auto-consistente.

- [ ] **Step 4: Rodar o script contra o build local**

```
D:\GitHub\firebird\temp\x64\Release\firebird\isql.exe -u SYSDBA -p masterkey ^
  -i D:\GitHub\firebird\test_ltrim_zero.sql -o D:\GitHub\firebird\ltz_local_after.txt
```

Esperado: nenhuma linha de falha no relatório final do script. Conferir explicitamente que `9.1`, `9.2` e os casos reescritos do bloco 6b aparecem como OK.

Lembrete: `isql` embedded não abre banco que o servidor está usando, e vice-versa. Se aparecer "O arquivo já está sendo usado por outro processo", parar o serviço ou usar outro caminho de banco.

- [ ] **Step 5: Atualizar a documentação da collation**

Em `doc/README.ltrim_zero.md`:
- descrever a ordem: tamanho do normalizado primeiro, depois byte a byte em caixa alta;
- reproduzir a tabela da seção 3 da spec (valor gravado, normalizado, tamanho, posição);
- deixar explícito que a igualdade não mudou e que `'000123' = '123'` continua verdadeiro;
- registrar que `STARTING WITH` e `LIKE 'x%'` sobre coluna indexada passam a varrer o índice inteiro, com resultado correto e plano pior;
- registrar que `BETWEEN` mudou de significado além do caso CNPJ: `BETWEEN '9' AND '11'` passa a incluir todo valor de tamanho 1 maior que `9`, inclusive letras, antes de chegar em `10`;
- registrar o limite de cauda: em coluna mais larga que `MAX_KEY = 8192`, a chave já era truncada, e o prefixo custa 2 bytes a mais de cauda, então dois valores que difiram só nos 2 últimos bytes passam a colidir em `GROUP BY`, `DISTINCT` e índice. Não alcança coluna de 20 bytes como a do domínio `TDR_CNPJ`;
- apontar para `doc/README.ltrim_zero_rollout.md` (criado na Task 6) para quem já tem índices no formato antigo.

Em `doc/ltrim_zero_code_review.md`: revisar as afirmações que descrevem a chave como sendo a string normalizada e o `INTL_KEY_PARTIAL` como caso não especial, alinhando com o driver novo.

- [ ] **Step 6: Corrigir a spec**

Em `docs/superpowers/specs/2026-08-02-ltrim-zero-numeric-order-design.md`:

Na seção 6, terceiro bullet, trocar

```
- Qualquer outro banco do servidor que use `ISO8859_1_LTRIM_ZERO`, `WIN1252_LTRIM_ZERO`,
  `UTF8_LTRIM_ZERO`, `NONE_LTRIM_ZERO` ou `DOS850_LTRIM_ZERO`.
```

por

```
- Qualquer outro banco do servidor que use `ISO8859_1_LTRIM_ZERO` ou `WIN1252_LTRIM_ZERO`.
  São as duas únicas collations registradas, tanto no `fbintl` (`src/intl/ld.cpp:386,445`)
  quanto no módulo standalone (`fbltrimzero.conf`). O nome local pode ser outro: quem
  manda é o `RDB$COLLATIONS.RDB$BASE_COLLATION_NAME`, que guarda o nome do
  `FROM EXTERNAL` (`DdlNodes.epp:3977`).
```

Ainda na seção 6, trocar a frase final

```
A decisão entre os dois entra no plano de implementação, medindo o tempo dos dois
caminhos.
```

por

```
Não há decisão a medir: um rebuild seletivo deixaria os índices de constraint no formato
antigo enquanto o dll já fala o novo, e esse é o estado de resultado errado, não o estado
lento. O caminho é backup e restore.
```

Na seção 4.1, último parágrafo, trocar

```
Já é assim hoje; os 2 bytes só encolhem a margem.
```

por

```
Já é assim hoje, mas os 2 bytes não só encolhem a margem: acima de `MAX_KEY` o corte
é na cauda, então dois valores que difiram apenas nos 2 últimos bytes passam a produzir
a mesma chave e colidem em `GROUP BY`, `DISTINCT` e índice, onde a collation padrão
ainda os separa. Só alcança coluna mais larga que 8192 bytes; a do domínio `TDR_CNPJ`
tem 20.
```

- [ ] **Step 7: Commit**

```bash
git add test_ltrim_zero.sql doc/README.ltrim_zero.md doc/ltrim_zero_code_review.md docs/superpowers/specs/2026-08-02-ltrim-zero-numeric-order-design.md
git commit -m "docs(intl): numeric ordering in the SQL suite and the collation docs"
```

---

## Task 5: Regerar o `fbltrimzero.dll` com o código novo e revalidar na instalação estoque

**Files:**
- Nenhum arquivo novo. Roda `builds/win32/make_ltrimzero.bat` (Task 1) sobre o driver da Task 2.

**Interfaces:**
- Consumes: `builds/win32/make_ltrimzero.bat`, `src/intl/lc_ltrim_zero.cpp` já alterado, `test_ltrim_zero.sql` já atualizado.
- Produces: `builds\win32\ltrimzero\fbltrimzero.dll` com a ordem nova, pronto para a janela de troca da Task 6.

- [ ] **Step 1: Regerar o módulo**

```
cmd /c 'cd /d D:\GitHub\firebird\builds\win32 & set PATH=.;%PATH% & call setenvvar.bat & call make_ltrimzero.bat'
```

Esperado: mesma saída de `dumpbin` da Task 1 (dois exports sem decoração, só `KERNEL32.dll` como dependência).

- [ ] **Step 2: Instalar num Firebird estoque e rodar a suíte SQL**

Repetir os passos 6 e 7 da Task 1, agora com o dll novo, contra **um banco criado do zero** (o script já cria o seu).

```
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" -u SYSDBA -p masterkey ^
  -i D:\GitHub\firebird\test_ltrim_zero.sql -o D:\GitHub\firebird\ltz_stock_after.txt
```

Esperado: nenhuma falha, e `9.1` agora com `9,a,B,10,100`.

Isso é o que fecha a frente 2: o mesmo comportamento sai do `fbintl.dll` do nosso build e do `fbltrimzero.dll` sobre engine estoque.

- [ ] **Step 3: Comparar os dois relatórios**

```powershell
Compare-Object (Get-Content D:\GitHub\firebird\ltz_local_after.txt) `
               (Get-Content D:\GitHub\firebird\ltz_stock_after.txt)
```

Esperado: só diferenças de caminho de arquivo e de tempo, nenhuma diferença de resultado. Diferença de resultado significa que o dll standalone e o `fbintl` embutido divergiram, o que só pode vir de flags de compilação; investigar antes de seguir.

- [ ] **Step 4: Guardar o binário validado**

```powershell
$stamp = 'numeric-order'
Copy-Item 'D:\GitHub\firebird\builds\win32\ltrimzero\fbltrimzero.dll' `
          "D:\u\banco\scherer\fbltrimzero_$stamp.dll"
```

O runbook da Task 6 referencia esse arquivo.

- [ ] **Step 5: Commit**

Nada de código muda aqui. Se o passo 3 exigiu ajuste de flags no `.bat`:

```bash
git add builds/win32/make_ltrimzero.bat
git commit -m "build(intl): align the standalone module flags with fbintl"
```

Caso contrário, seguir sem commit.

---

## Task 6: Inventário de rollout e runbook do `SCHERER_001`

Esta task não é TDD: o deliverable é um documento operacional mais o script SQL que o alimenta, no mesmo formato do `README_collate_tdr_cnpj.md` que já existe em `D:\u\banco\scherer\`. O critério de aceite é o inventário rodar contra o `SCHERER_001.FDB` e produzir números conferíveis.

**Files:**
- Create: `doc/ltrim_zero_rollout_inventory.sql`
- Create: `doc/README.ltrim_zero_rollout.md`

**Interfaces:**
- Consumes: o fato de que `RDB$COLLATIONS.RDB$BASE_COLLATION_NAME` guarda o nome do `FROM EXTERNAL` (`src/dsql/DdlNodes.epp:3977`); a chave nova de `len + 2` bytes por segmento; o binário guardado na Task 5.
- Produces: `doc/README.ltrim_zero_rollout.md`, referenciado pelo `doc/README.ltrim_zero.md` da Task 4.

- [ ] **Step 1: Escrever o inventário**

Criar `doc/ltrim_zero_rollout_inventory.sql`. O ponto crítico: **nada aqui procura pelo nome local da collation.** `CREATE COLLATION ... FROM EXTERNAL` deixa o DBA escolher qualquer nome local, e o próprio harness de teste deste repositório faz `CREATE COLLATION LTZ FOR WIN1252 FROM EXTERNAL ('WIN1252_LTRIM_ZERO')`. Um inventário que procurasse por `ISO8859_1_LTRIM_ZERO` em `RDB$COLLATION_NAME` perderia esse banco em silêncio, e índice perdido depois da troca é índice que devolve resultado errado.

```sql
/*
 * LTRIM_ZERO rollout inventory.
 *
 * Run with isql against every database on the server before swapping
 * fbltrimzero.dll. Everything is resolved through
 * RDB$COLLATIONS.RDB$BASE_COLLATION_NAME, which holds the name given to
 * CREATE COLLATION ... FROM EXTERNAL (src/dsql/DdlNodes.epp:3977), never
 * through the local collation name: the DBA is free to call it anything.
 */

SET LIST ON;

/* ---------------------------------------------------------------- */
/* 1. Collations in this database that come from the LTRIM_ZERO      */
/*    module, whatever local name they were given.                   */
/* ---------------------------------------------------------------- */

SELECT TRIM(C.RDB$COLLATION_NAME)      AS LOCAL_NAME,
       TRIM(C.RDB$BASE_COLLATION_NAME) AS EXTERNAL_NAME,
       C.RDB$COLLATION_ID              AS COLL_ID,
       C.RDB$CHARACTER_SET_ID          AS CS_ID,
       C.RDB$COLLATION_ATTRIBUTES      AS ATTRS
FROM RDB$COLLATIONS C
WHERE TRIM(COALESCE(C.RDB$BASE_COLLATION_NAME, C.RDB$COLLATION_NAME))
      IN ('ISO8859_1_LTRIM_ZERO', 'WIN1252_LTRIM_ZERO');

/* ---------------------------------------------------------------- */
/* 2. Columns using one of those collations.                         */
/*    The effective collation of a column is the override in         */
/*    RDB$RELATION_FIELDS when present, otherwise the domain's in    */
/*    RDB$FIELDS. Collation ids are only unique inside a charset, so */
/*    the join carries both.                                         */
/* ---------------------------------------------------------------- */

WITH LTZ AS (
    SELECT C.RDB$COLLATION_ID AS COLL_ID,
           C.RDB$CHARACTER_SET_ID AS CS_ID
    FROM RDB$COLLATIONS C
    WHERE TRIM(COALESCE(C.RDB$BASE_COLLATION_NAME, C.RDB$COLLATION_NAME))
          IN ('ISO8859_1_LTRIM_ZERO', 'WIN1252_LTRIM_ZERO')
)
SELECT TRIM(RF.RDB$RELATION_NAME) AS REL,
       TRIM(RF.RDB$FIELD_NAME)    AS FLD,
       TRIM(RF.RDB$FIELD_SOURCE)  AS DOMAIN_NAME,
       F.RDB$FIELD_LENGTH         AS BYTES
FROM RDB$RELATION_FIELDS RF
JOIN RDB$FIELDS F ON F.RDB$FIELD_NAME = RF.RDB$FIELD_SOURCE
JOIN LTZ ON LTZ.CS_ID = F.RDB$CHARACTER_SET_ID
        AND LTZ.COLL_ID = COALESCE(RF.RDB$COLLATION_ID, F.RDB$COLLATION_ID, 0)
ORDER BY 1, 2;

/* ---------------------------------------------------------------- */
/* 3. Index segments over those columns. Every one of these holds    */
/*    keys in the old format and is wrong until rebuilt.             */
/*    RDB$RELATION_CONSTRAINTS tells which ones cannot be            */
/*    deactivated with ALTER INDEX (trig.h:1377-1378).               */
/* ---------------------------------------------------------------- */

WITH LTZ AS (
    SELECT C.RDB$COLLATION_ID AS COLL_ID,
           C.RDB$CHARACTER_SET_ID AS CS_ID
    FROM RDB$COLLATIONS C
    WHERE TRIM(COALESCE(C.RDB$BASE_COLLATION_NAME, C.RDB$COLLATION_NAME))
          IN ('ISO8859_1_LTRIM_ZERO', 'WIN1252_LTRIM_ZERO')
),
COLS AS (
    SELECT RF.RDB$RELATION_NAME AS REL, RF.RDB$FIELD_NAME AS FLD
    FROM RDB$RELATION_FIELDS RF
    JOIN RDB$FIELDS F ON F.RDB$FIELD_NAME = RF.RDB$FIELD_SOURCE
    JOIN LTZ ON LTZ.CS_ID = F.RDB$CHARACTER_SET_ID
            AND LTZ.COLL_ID = COALESCE(RF.RDB$COLLATION_ID, F.RDB$COLLATION_ID, 0)
)
SELECT TRIM(I.RDB$INDEX_NAME)                  AS IDX,
       TRIM(I.RDB$RELATION_NAME)               AS REL,
       TRIM(S.RDB$FIELD_NAME)                  AS SEG,
       S.RDB$FIELD_POSITION                    AS POS,
       COALESCE(I.RDB$UNIQUE_FLAG, 0)          AS IS_UNIQUE,
       COALESCE(I.RDB$INDEX_TYPE, 0)           AS IS_DESC,
       COALESCE(I.RDB$INDEX_INACTIVE, 0)       AS INACTIVE,
       TRIM(COALESCE(RC.RDB$CONSTRAINT_TYPE, 'NONE')) AS CONSTRAINT_TYPE
FROM RDB$INDEX_SEGMENTS S
JOIN RDB$INDICES I ON I.RDB$INDEX_NAME = S.RDB$INDEX_NAME
JOIN COLS ON COLS.REL = I.RDB$RELATION_NAME AND COLS.FLD = S.RDB$FIELD_NAME
LEFT JOIN RDB$RELATION_CONSTRAINTS RC ON RC.RDB$INDEX_NAME = I.RDB$INDEX_NAME
ORDER BY 2, 1, 4;

/* ---------------------------------------------------------------- */
/* 4. Expression indexes. Their result can carry the collation and   */
/*    they never show up in RDB$INDEX_SEGMENTS (btr.cpp:2059), so    */
/*    the source of each one has to be read by hand and matched      */
/*    against the column list from query 2.                          */
/* ---------------------------------------------------------------- */

SELECT TRIM(I.RDB$INDEX_NAME)    AS IDX,
       TRIM(I.RDB$RELATION_NAME) AS REL,
       I.RDB$EXPRESSION_SOURCE   AS EXPR
FROM RDB$INDICES I
WHERE I.RDB$EXPRESSION_BLR IS NOT NULL
ORDER BY 2, 1;

/* ---------------------------------------------------------------- */
/* 5. Key size headroom. Each segment of an affected index grows by  */
/*    2 bytes, a descending index adds 1 more, and a compound index  */
/*    amplifies that by roughly 25% of stuff bytes (btr.cpp:1727).   */
/*    The limit is page_size / 4 (Database.h:652-655).               */
/*                                                                   */
/*    This is a rough upper bound, not the exact engine computation: */
/*    anything it flags has to be looked at, anything it clears by a */
/*    wide margin is safe.                                           */
/* ---------------------------------------------------------------- */

WITH LTZ AS (
    SELECT C.RDB$COLLATION_ID AS COLL_ID,
           C.RDB$CHARACTER_SET_ID AS CS_ID
    FROM RDB$COLLATIONS C
    WHERE TRIM(COALESCE(C.RDB$BASE_COLLATION_NAME, C.RDB$COLLATION_NAME))
          IN ('ISO8859_1_LTRIM_ZERO', 'WIN1252_LTRIM_ZERO')
),
SEG AS (
    SELECT I.RDB$INDEX_NAME AS IDX,
           I.RDB$RELATION_NAME AS REL,
           COALESCE(I.RDB$INDEX_TYPE, 0) AS IS_DESC,
           COUNT(*) AS SEGMENTS,
           SUM(F.RDB$FIELD_LENGTH) AS RAW_BYTES,
           SUM(CASE WHEN LTZ.COLL_ID IS NULL THEN 0 ELSE 2 END) AS EXTRA_BYTES
    FROM RDB$INDEX_SEGMENTS S
    JOIN RDB$INDICES I ON I.RDB$INDEX_NAME = S.RDB$INDEX_NAME
    JOIN RDB$RELATION_FIELDS RF ON RF.RDB$RELATION_NAME = I.RDB$RELATION_NAME
                               AND RF.RDB$FIELD_NAME = S.RDB$FIELD_NAME
    JOIN RDB$FIELDS F ON F.RDB$FIELD_NAME = RF.RDB$FIELD_SOURCE
    LEFT JOIN LTZ ON LTZ.CS_ID = F.RDB$CHARACTER_SET_ID
                 AND LTZ.COLL_ID = COALESCE(RF.RDB$COLLATION_ID, F.RDB$COLLATION_ID, 0)
    GROUP BY 1, 2, 3
)
SELECT TRIM(IDX) AS IDX, TRIM(REL) AS REL, SEGMENTS, RAW_BYTES, EXTRA_BYTES,
       CAST((RAW_BYTES + EXTRA_BYTES + IS_DESC)
            * (CASE WHEN SEGMENTS > 1 THEN 1.25 ELSE 1.0 END) AS INTEGER) AS EST_KEY_BYTES,
       (SELECT MON$PAGE_SIZE / 4 FROM MON$DATABASE) AS KEY_LIMIT
FROM SEG
WHERE EXTRA_BYTES > 0
ORDER BY 6 DESC;
```

- [ ] **Step 2: Rodar o inventário contra o banco do cliente e conferir os números**

```
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" -u SYSDBA -p masterkey ^
  D:\u\banco\scherer\SCHERER_001.FDB ^
  -i D:\GitHub\firebird\doc\ltrim_zero_rollout_inventory.sql ^
  -o D:\u\banco\scherer\rollout_inventory.txt
```

Conferências esperadas, contra os números já levantados:
- query 1 devolve pelo menos `ISO8859_1_LTRIM_ZERO`;
- query 2 devolve 368 colunas do domínio `TDR_CNPJ`;
- query 3 devolve 421 segmentos;
- query 4 devolve 38 índices de expressão;
- query 5 mostra `KEY_LIMIT = 4096` (página 16384) e `EST_KEY_BYTES` bem abaixo disso, porque as colunas do domínio têm 20 bytes.

Divergência em qualquer um desses cinco números é motivo para parar e entender antes de escrever o runbook. Repetir o inventário em cada outro banco do servidor.

- [ ] **Step 3: Escrever o runbook**

Criar `doc/README.ltrim_zero_rollout.md` cobrindo, na ordem:

1. **O que quebra.** Todo índice sobre coluna com a collation guarda chave no formato antigo. Depois de trocar o dll esses índices descrevem a ordem velha e devolvem resultado errado até serem reconstruídos.
2. **Por que backup e restore, e não `ALTER INDEX`.** `ALTER INDEX ... INACTIVE` é recusado para índice de constraint (`trig.h:1377-1378`, `ini.epp:189`), e boa parte dos 421 segmentos é de PK, UNIQUE ou FK. Um rebuild seletivo deixaria esses índices no formato antigo com o dll novo, que é o estado de resultado errado, não o estado lento.
3. **Nenhuma escrita entre trocar o dll e terminar o rebuild.** Não é só consulta: a checagem de chave estrangeira monta chave nova e procura no índice do parceiro, ainda no formato antigo (`idx.cpp:1942-1971`), então `INSERT` e `UPDATE` nessa janela furam integridade referencial e unicidade sem reclamar. A janela é de indisponibilidade total, não de somente leitura.
4. **Sequência da janela**, com os comandos exatos:
   - parar a aplicação e conferir que não há attachment (`SELECT COUNT(*) FROM MON$ATTACHMENTS`), lembrando que `gbak -PAR` abre uma conexão por worker e infla esse contador;
   - `gbak -b` do banco atual (o backup também é o rollback);
   - parar o serviço, guardar `fbltrimzero.dll` como `.bak`, copiar `fbltrimzero_numeric-order.dll` da Task 5, subir o serviço;
   - `gbak -c -PAR 5 -v -y` restaurando o backup por cima de um caminho novo. O restore reconstrói todo índice chamando `string_to_key` de novo, que agora é o novo;
   - renomear os arquivos, subir a aplicação.
5. **Validação depois do restore**, reaproveitando o que já foi usado na conversão do domínio: contagem de metadados (1242 tabelas, 885 procedures, 1812 triggers, 2231 índices, 7 índices inativos, 2 triggers com BLR inválido, 368 colunas no domínio, 38 índices de expressão), `ENTIDADE` com 544149 linhas, `SUM(CNPJ) = 6151677092957447095`, e índice contra varredura (`col` versus `col || ''`) batendo em igualdade, `BETWEEN` e `STARTING WITH`. Acrescentar a conferência nova: `SELECT V FROM ... ORDER BY V` com valores de larguras diferentes saindo na ordem por tamanho, e a faixa de raiz de CNPJ não trazendo CPF de 11 dígitos.
6. **`gfix -v` entre a troca do dll e o fim do restore reporta corrupção de índice.** É esperado e não indica problema novo.
7. **Rollback**: restaurar `fbltrimzero.dll.bak` e voltar os renomes de arquivo. Enquanto ninguém tiver gravado no banco novo, é reversível sem perda.
8. **Efeitos permanentes a comunicar ao cliente**: `STARTING WITH` e `LIKE 'x%'` sobre coluna do domínio passam a varrer o índice inteiro (resultado certo, plano pior, e o otimizador ainda acha a faixa seletiva por `REDUCE_SELECTIVITY_FACTOR_STARTING` em `Retrieval.cpp:990`); `BETWEEN` muda de significado além do caso CNPJ, e onde a coluna mistura número e texto vale revisar as faixas; relatórios que dependiam da ordem alfabética mudam.
9. **Outros bancos do servidor**: o dll é compartilhado. Rodar o inventário em cada banco e incluir no mesmo agendamento todo banco que a query 1 não devolver vazio.

- [ ] **Step 4: Commit**

```bash
git add doc/ltrim_zero_rollout_inventory.sql doc/README.ltrim_zero_rollout.md
git commit -m "docs(intl): rollout inventory and runbook for the numeric order"
```

---

## Fechamento

Depois da Task 6, rodar a bateria completa uma vez, na ordem:

```
cmd /c 'cd /d D:\GitHub\firebird\builds\win32 & set PATH=.;%PATH% & call make_boot.bat & call make_all.bat'
D:\GitHub\firebird\temp\x64\Release\firebird\engine_test.exe --run_test=IntlSuite/LtrimZeroKeySuite
D:\GitHub\firebird\temp\x64\Release\firebird\engine_test.exe --run_test=EngineSuite/LtrimZeroSuite
D:\GitHub\firebird\temp\x64\Release\firebird\isql.exe -u SYSDBA -p masterkey -i D:\GitHub\firebird\test_ltrim_zero.sql
```

Os três verdes, mais o inventário rodando limpo no `SCHERER_001`, fecham o plano. A execução da janela de troca no cliente é decisão do usuário, não parte deste plano.

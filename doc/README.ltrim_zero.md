# LTRIM_ZERO collation

Collation single byte para `WIN1252` e `ISO8859_1` que ignora zeros e espaços
**à esquerda** e compara sem diferenciar maiúsculas de minúsculas no intervalo
ASCII.

```
"00000A" = "0A" = "A" = "    A" = "a" = "000a  "
```

Implementação: `src/intl/lc_ltrim_zero.cpp`
Registro no driver: `src/intl/ld.cpp`
Registro em runtime: `builds/install/misc/fbintl.conf`
Suíte de testes: `test_ltrim_zero.sql` (rodar com `run_test.bat`)

---

## 1. DDL

```sql
CREATE COLLATION WIN1252_LTRIM_ZERO FOR WIN1252
    FROM EXTERNAL ('WIN1252_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE;

CREATE COLLATION ISO8859_1_LTRIM_ZERO FOR ISO8859_1
    FROM EXTERNAL ('ISO8859_1_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE;
```

**As duas cláusulas são obrigatórias na prática:**

- `PAD SPACE` é o que faz coluna `CHAR` funcionar. Sem ela, `'A'` guardado em
  `CHAR(5)` é fisicamente `"A    "` e nunca casa com o literal `'A'`.
- `CASE INSENSITIVE` é o que mantém `SIMILAR TO` alinhado com `=`, `LIKE` e
  `CONTAINING`. `SIMILAR TO` não passa pelo `texttype_fn_canonical`: ele compila
  para RE2 e lê a insensibilidade de caixa direto do atributo da collation
  (`src/jrd/Collation.cpp:132`). Sem o atributo, `SIMILAR TO` fica
  case-sensitive enquanto todo o resto não fica.

`ACCENT INSENSITIVE` é rejeitado de propósito: a comparação é ASCII-only, e
aceitar o atributo faria `SIMILAR TO` dobrar acentos enquanto `=` não dobra.

Uso típico:

```sql
CREATE DOMAIN CODIGO_LTRIM AS VARCHAR(50)
    CHARACTER SET WIN1252 COLLATE WIN1252_LTRIM_ZERO;
```

---

## 2. Semântica exata

Chamando `N(x)` a forma normalizada usada tanto pela comparação quanto pela
chave de índice:

1. remove espaços à direita (só quando `PAD SPACE`);
2. remove `'0'` e `' '` à esquerda, em qualquer intercalação, até o primeiro
   caractere que não seja nenhum dos dois;
3. converte `a-z` para `A-Z`.

Consequências:

| entrada | `N` | observação |
|---|---|---|
| `'00000A'`, `'0A'`, `'A'`, `'    A'`, `'a'`, `'000a  '` | `A` | mesma classe |
| `'0 A 0'` | `A 0` | o corte para no primeiro caractere real |
| `'A00'`, `'00A00'` | `A00` | zero à direita é significativo |
| `'000'`, `'   '`, `' 0 '`, `''` | vazio | tudo que só tem zero/espaço vira string vazia |
| `NULL` | - | `NULL` continua `NULL`, não entra na classe vazia |

A relação é uma equivalência de verdade (`N(a) == N(b)` para uma função pura
`N`), então `=`, `DISTINCT`, `GROUP BY`, `UNIQUE` e a ordem do índice sempre
concordam - exceto no limite de `MAX_KEY` documentado na seção 3, onde
`DISTINCT` decide a partir da chave truncada e pode divergir de `=`. Por isso
a collation **não** precisa de `TEXTTYPE_SEPARATE_UNIQUE`.

A chave de índice deixou de ser a string normalizada sozinha. Agora é

```
[2 bytes: tamanho de N(x), big-endian][bytes de N(x), em caixa alta]
```

O prefixo de tamanho é o que faz a ordenação ser por tamanho primeiro (ver
seção 3). Duas entradas continuam na mesma classe de equivalência exatamente
quando `N(a) == N(b)`, porque strings normalizadas iguais têm tamanho igual,
então a chave continua sendo uma função pura de `N(x)`, e `=`/`DISTINCT`/
`GROUP BY`/`UNIQUE` continuam concordando entre si e com a ordem do índice,
com a mesma exceção de `MAX_KEY` da seção 3.

> Ao editar o driver, mantenha `texttype_fn_compare` e
> `texttype_fn_string_to_key` no mesmo `normalize_bounds`. Se os dois
> divergirem, `UNIQUE` e `DISTINCT` param de concordar com `=` em silêncio.

---

## 3. Limites conhecidos

**A ordem é pelo tamanho da forma normalizada primeiro, depois byte a byte
sobre a forma normalizada, em caixa alta.** `'9' < '10'`, batendo com o que
quem adota uma collation "tira zero à esquerda" costuma esperar. Dentro do
mesmo tamanho, a ordem cai para a ordem de byte simples, então dígito vem
antes de letra (`'9' = 0x39 < 'A' = 0x41`). O tamanho que conta é o de `N(x)`,
a forma normalizada, não o do valor gravado, o que é o que põe `'9'` e
`'0000009'` na mesma posição:

| valor gravado | normalizado | tamanho | posição |
|---|---|---|---|
| `'000'` | (vazio) | 0 | 1ª |
| `'9'` | `9` | 1 | 2ª, empatado |
| `'0000009'` | `9` | 1 | 2ª, empatado |
| `'A'` | `A` | 1 | 4ª |
| `'10'` | `10` | 2 | 5ª |
| `'0001A34'` | `1A34` | 4 | 6ª |
| `'12345678901'` | `12345678901` | 11 | 7ª |
| `'12345678000199'` | `12345678000199` | 14 | 8ª |

Igualdade não mudou: `'000123' = '123'` continua verdadeiro, e a classe de
equivalência continua sendo a forma normalizada. Valores que só diferem em
zeros à esquerda continuam comparando iguais, e ainda colidem num índice
`UNIQUE`, exatamente como antes.

**`BETWEEN` mudou de significado além do caso CNPJ que motivou a ordem por
tamanho.** `BETWEEN '9' AND '11'` agora inclui todo valor de tamanho
normalizado 1 maior que `'9'`, inclusive letras, antes de chegar em `'10'`:
`'A'`, `'B'`, `'Z'` satisfazem o intervalo, porque empatam com `'9'` no
tamanho e vencem no byte, mas continuam mais curtos que o limite superior
`'11'`, de tamanho 2. Quem filtra uma coluna de largura mista com `BETWEEN`
precisa levar isso em conta, não só o caso de busca de raiz de CNPJ.

**Acima de `MAX_KEY` (8192 bytes), `DISTINCT` (e empate de `ORDER BY`) podem
parar de distinguir dois valores que difiram só nos 2 últimos bytes; `GROUP
BY` não.** `MAX_KEY` é uma constante de compilação (`constants.h:195`), então
esse limite não depende do tamanho de página: verificado idêntico em página
de 8192 e de 32768. O motor sempre cortou a chave de sort de um valor mais
largo que `MAX_KEY` no comprimento bruto do campo, truncando a cauda em
silêncio (`intl.cpp:1002-1019`).

A truncagem depende da **largura declarada da coluna**, não só do tamanho
normalizado de um valor: `INTL_key_length` dimensiona o espaço de chave de
sort a partir do comprimento bruto do campo, igual para toda linha da coluna.
Tamanho normalizado 8191 é necessário para a truncagem ser possível (abaixo
disso, nenhuma largura de coluna dispara), mas não suficiente: numa coluna
`VARCHAR(12000)`, `dstLen` é 12000 para toda linha, então a truncagem só
começa em tamanho normalizado 11999 (verificado: 11998 não colide em
`DISTINCT`, 11999 colide), não em 8191.

`DISTINCT` decide igualdade a partir da chave truncada, mas não por
`SortedStream::compareKeys` - essa função só é chamada por merge join
(`MergeJoin.cpp:273`), sem relação com `DISTINCT`. O caminho real: quando
`FLAG_PROJECT` está ligado, `SortedStream::init` (`SortedStream.cpp:190`)
passa `RecordSource::rejectDuplicate` (`RecordSource.h:104`, devolve `true`
incondicionalmente) como callback de duplicata do sort, disparado por
`DO_32_COMPARE` sobre a chave crua em `sort.cpp:1301-1312` sempre que duas
chaves adjacentes comparam iguais byte a byte. Não há nenhuma revalidação de
valor nesse caminho - nem o retorno de `FLAG_KEY_VARY` do CORE-4909 que
`SortedStream::compareKeys` tem (`SortedStream.cpp:286-317`). Por isso dois
valores diferentes que colidam na chave truncada se fundem em `DISTINCT`.

`GROUP BY` não: ele revalida com um compare real de valor ao decidir se um
grupo novo começou (`AggregatedStream.cpp:308-345`, `lookForChange`,
`MOV_compare` na linha 345), então continua correto independente da
truncagem da chave, sempre.

Índice de verdade nunca alcança esse regime, e a margem é folgada: `CREATE
INDEX` calcula `key_length = ROUNDUP(INTL_key_length(len) + 1, 8)` (o `+1` é
o byte indicador de NULL, `idx.cpp:876-877`) e recusa quando isso é maior ou
igual a `page_size / 4` (`idx.cpp:879`, `Database.h:654`). No maior tamanho
de página (32768, teto 8192), verificado: `VARCHAR(8181)` cria, `VARCHAR(8182)`
falha com "key size exceeds implementation restriction", e `VARCHAR(8190)`
falha antes ainda, numa checagem mais grosseira de `MAX_KEY` no DSQL
(`DdlNodes.epp`) com "key size too big for index". A coluna mais larga ainda
indexável é `VARCHAR(8181)`, 10 bytes inteiros abaixo de onde a truncagem de
chave de sort começaria. Só chave de sort (`ORDER BY`, `GROUP BY`,
`DISTINCT`), que não tem teto de `page_size / 4`, alcança esse regime. A
menor coluna capaz de chegar lá é bem mais larga que os 20 bytes do domínio
`TDR_CNPJ`; a base do cliente não é alcançada.

**Case-insensitive só em ASCII.** Registrada em WIN1252/ISO8859_1, mas
`'é' <> 'É'` na comparação. `UPPER()` e `LOWER()` continuam corretos com acento,
porque o driver deliberadamente não instala `texttype_fn_str_to_upper` /
`_str_to_lower` e deixa o ICU do motor cuidar disso.

**`LIKE` / `CONTAINING` / `SIMILAR TO` não são zero-insensitive.** Esses
operadores passam pelo `texttype_fn_canonical`, que é um mapeamento
**por caractere** e por construção não consegue remover zeros à esquerda. A
forma canônica aqui é só o uppercase. Então `'00000A' LIKE 'A'` é falso, embora
`'00000A' = 'A'` seja verdadeiro. Se precisar de busca por prefixo
zero-insensitive, use índice por expressão ou uma coluna normalizada
persistida.

**`STARTING WITH` e `LIKE 'x%'` sobre coluna indexada agora varrem o índice
inteiro, não só a faixa que casa.** Como a chave carrega um prefixo de
tamanho, a chave de um prefixo deixou de ser prefixo em bytes da chave do
valor inteiro, então uma chave parcial não consegue mais descrever "starts
with" de jeito nenhum. `string_to_key` devolve chave vazia para qualquer
prefixo nesse caso, o que o motor trata como "nenhum filtro vindo do
índice": ele percorre toda entrada e reaplica o predicado depois do fetch.
Isso antes só acontecia para prefixo que normaliza inteiro para vazio (como
`'00'`); agora acontece para todo `STARTING WITH` e `LIKE 'x%'`,
independente do prefixo. O resultado continua correto: plano natural e
plano por índice devolvem o mesmo conjunto, nenhuma linha se perde, e isso
é coberto pelos asserts 8.6, 8.6b, 8.6c e 8.7. O plano piora, porém: o
otimizador ainda estima a faixa como seletiva e não sabe que a própria
varredura agora é completa.

**`UNIQUE` segue a collation.** Inserir `'0A'` depois de `'A'` é violação de
chave. É a semântica pedida, mas precisa estar clara para quem modela.

---

## 4. Deploy em base existente - leia antes de trocar a lib

A forma normalizada define os bytes da chave de índice. **Qualquer mudança na
normalização invalida todo índice já construído sobre uma coluna
`LTRIM_ZERO`**, e o motor não valida índice contra versão de collation: o
sintoma é linha não encontrada, sem erro nenhum.

Ao substituir `fbintl.dll` / `libfbintl.so` numa base que já tem esses índices:

- `gbak` backup/restore, **ou**
- `ALTER INDEX ... INACTIVE` seguido de `ALTER INDEX ... ACTIVE` em todos os
  índices envolvidos (inclusive os implícitos de `PRIMARY KEY` / `UNIQUE`).

Ver `doc/README.ltrim_zero_rollout.md` para quem já tem índice construído no
formato de chave antigo (sem o prefixo de tamanho) e precisa do inventário e
do roteiro para a troca.

---

## 5. Build

Nenhuma lista de fontes precisa ser mantida à mão, exceto no MSVC:

- POSIX: `builds/posix/make.shared.variables:4-7,137` faz glob de
  `src/intl/*.cpp`.
- CMake: `src/CMakeLists.txt:518` faz glob, **sem `CONFIGURE_DEPENDS`**. Build
  tree CMake já existente continua gerando um `fbintl` sem o objeto novo até
  alguém rodar `cmake` de novo.
- MSVC: `builds/win32/msvc15/intl.vcxproj` e `.filters` listam o arquivo
  explicitamente.
- `builds/posix/fbintl.vers` não precisa de alteração: `LCLTRIMZERO_init` só é
  referenciado de dentro do próprio `fbintl`, exportá-lo seria errado.

---

## 6. Testes

São dois, com escopos que não se sobrepõem.

### 6.1 Semântica - `test_ltrim_zero.sql`

```bat
msbuild builds\win32\msvc15\intl.vcxproj /p:Configuration=Release /p:Platform=x64
run_test.bat
```

Auto-verificável: cada check grava uma linha em `TST` e o rodapé imprime
`SUMMARY` e `VERDICT`. Passa quando sai `*** SUITE PASSED ***` com `FAILED = 0`.
Checks marcados `I` (info) documentam os limites da seção 3 e nunca reprovam.

Cobre: classe de equivalência, `PAD SPACE` em `CHAR`/`VARCHAR`, variante
`NO PAD`, classe vazia versus `NULL`, concordância entre plano natural e plano
por índice (`=`, `STARTING WITH`, `ORDER BY`, join), `UNIQUE`, valores de 32600
bytes, chaves normalizadas acima de `MAX_KEY` comparadas contra a collation
padrão como controle, `UPPER`/`LOWER` com acento, `LIKE`/`CONTAINING`/`SIMILAR
TO`, e o registro em `ISO8859_1`.

### 6.2 Integração - `src/jrd/tests/LtrimZeroCollationTest.cpp`

Entra no `engine_test` (Boost.Test). No POSIX é pego pelo glob
(`make.shared.variables:93`); no MSVC está listado em `engine_test.vcxproj`.

```bat
msbuild builds\win32\msvc15\engine_test.vcxproj /p:Configuration=Release /p:Platform=x64
temp\x64\Release\firebird\engine_test.exe --run_test=EngineSuite/LtrimZeroSuite
```

```bash
make run_tests                                     # tudo
./gen/.../engine_test --run_test=EngineSuite/LtrimZeroSuite   # só esta suíte
```

Cobre o que um script isql não alcança:

| Caso | O que exercita |
|---|---|
| `ConcurrentVolumeAndValidation` | 8 attachments simultâneos inserindo 50000 linhas numa tabela indexada, depois validação online do banco. Split de página, b-tree multi-nível, compressão de prefixo entre nós |
| `DescendingIndexAndEmptyKey` | Índice `DESCENDING` com chave vazia (`btr.cpp:2941-2944`), navegação descendente contra sort ascendente invertido |
| `CompoundIndex` | `key_empty`/`key_nulls` por segmento em índice de duas colunas |
| `PrimaryAndForeignKey` | Colisão de PK entre `'A'` e `'000a  '`, FK de `'0A'` para `'A'`, delete de pai bloqueado por filho |
| `BackupRestoreRoundTrip` | `gbak` backup/restore pela Services API. Restore reconstrói todo índice chamando `string_to_key` de novo - é o caminho de deploy da seção 4 |
| `BlobAndTransliteration` | `CONTAINING` em `BLOB SUB_TYPE TEXT` com a collation, e comparação contra literais `_UTF8` / `_ISO8859_1` |

Todos comparam **identidade de linha** (lista ordenada de IDs) entre plano
natural e plano por índice, não apenas contagem.

O primeiro caso, `CollationIsInstalled`, roda antes de tudo e falha alto quando
o diretório de runtime está incompleto. Ele cria uma collation de **controle**
(`PXW_SPAN`, que mora no mesmo bloco do `fbintl.conf`) antes de tentar a
`LTRIM_ZERO`: se o controle também falhar, a mensagem diz que aquele tree não
consegue registrar nada vindo do `fbintl.conf` e que a suíte é inconclusiva -
em vez de parecer um bug da collation.

### 6.3 Linux e DEV_BUILD, via Docker

`doc/ltrim_zero_docker/` traz o harness usado para validar em Linux. Ele clona o
repositório de dentro do container - a árvore Windows tem CRLF e isso quebra o
`autogen.sh` - e depois sobrepõe apenas os arquivos que diferem do commit.

```bat
docker build -t fb-ltz-build doc\ltrim_zero_docker
docker volume create fb-ltz-src

REM Release
docker run --rm -v "%CD%:/repo:ro" -v "%CD%\doc\ltrim_zero_docker:/work:ro" ^
    -v fb-ltz-src:/src fb-ltz-build bash /work/run.sh release

REM DEV_BUILD: --enable-developer faz DefaultTarget=Debug
REM (builds/posix/Makefile.in:55-59) e compila com -DDEV_BUILD
REM (builds/posix/make.rules:63-67)
docker run --rm -v "%CD%:/repo:ro" -v "%CD%\doc\ltrim_zero_docker:/work:ro" ^
    -v fb-ltz-src:/src fb-ltz-build bash /work/run.sh developer
```

O volume `fb-ltz-src` mantém a árvore compilada entre execuções, então uma
segunda rodada não recompila tudo.

A passada `developer` é a verificação mais forte disponível: ela reativa o
`fb_assert` de `src/jrd/intl.cpp:398-399`, que exige que
`texttype_canonical_width` e `texttype_fn_canonical` sejam definidos juntos.
Se a collation errasse esse par, a rodada abortaria em vez de passar.

Resultado das duas passadas (Ubuntu 22.04, clang, x64):

| | Release | `--enable-developer` |
|---|---|---|
| `make` | ok | ok, 376 linhas com `-DDEV_BUILD` |
| `lc_ltrim_zero.o` gerado | sim, pelo glob, sem alterar build file | sim |
| `LCLTRIMZERO_init` no `libfbintl.so` | símbolo local, não exportado | idem |
| Suíte C++ | exit 0, sem erros | exit 0, sem erros |
| `make run_tests` completo | passou | passou |
| Validação online do banco | 0 erros | 0 erros |
| Suíte SQL | 55/55 | 55/55 |
| `fb_assert` de `intl.cpp:398-399` | inativo (Release) | **não disparou** |

---

## 7. Notas de implementação

O driver é *stateless*: nenhum estado mutável, `texttype_impl` fica `NULL` e não
há `texttype_fn_destroy`. Logo é reentrante e seguro sob concorrência alta.

`texttype_fn_compare` e `texttype_fn_string_to_key` são passe único, sem buffer
temporário e sem alocação. Isso é requisito, não otimização:

- os callbacks INTL rodam em toda comparação e em toda chave de índice, e o
  motor os invoca **sem** nenhuma barreira de exceção (`Jrd::TextType`), então
  eles não podem lançar;
- alocar do `getDefaultMemoryPool()` pegaria o mutex do pool global do processo
  a cada comparação, serializando as threads do SuperServer;
- `string_to_key` recebe `dstLen = 32767` no caminho de índice
  (`src/jrd/btr.cpp:2880`) independente do tamanho do valor, então preencher o
  resto do buffer custaria um `memset` de ~32 KB por chave. O motor usa apenas o
  comprimento retornado.

`texttype_canonical_width` e `texttype_fn_canonical` têm que ser definidos
juntos: `src/jrd/intl.cpp:398-399` tem um `fb_assert` sobre isso, e DEV_BUILD é
o build padrão de desenvolvedor no Linux. Em Release o assert some, mas deixar
`fn_canonical` nulo com width diferente de zero desliga o `TEXTTYPE_DIRECT_MATCH`
e joga o pattern matching no `memcpy` identidade, deixando `LIKE` case-sensitive
enquanto `=` é case-insensitive.

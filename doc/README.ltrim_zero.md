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
`N`), e a chave de índice **é** `N(x)`, então `=`, `DISTINCT`, `GROUP BY`,
`UNIQUE` e a ordem do índice sempre concordam. Por isso a collation **não**
precisa de `TEXTTYPE_SEPARATE_UNIQUE`.

> Ao editar o driver, mantenha `texttype_fn_compare` e
> `texttype_fn_string_to_key` no mesmo `normalize_bounds`. Se os dois
> divergirem, `UNIQUE` e `DISTINCT` param de concordar com `=` em silêncio.

---

## 3. Limites conhecidos

**Ordenação é lexicográfica, não numérica.** `'10' < '9'` porque `0x31 < 0x39`.
Quem adota uma collation "tira zero à esquerda" costuma esperar ordem numérica e
não vai receber. Comparação e índice concordam entre si.

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

`STARTING WITH` é consistente entre planos: o prefixo `'00'` gera chave parcial
vazia, o motor varre o índice inteiro e reavalia o predicado depois do fetch, de
modo que plano natural e plano por índice devolvem o mesmo conjunto. Nenhuma
linha é perdida.

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
| Suíte SQL | 52/52 | 52/52 |
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

# LTRIM_ZERO: ordenação numérica por padrão

> **Rename note.** This is a dated record, written while the collation was called
> `LTRIM_ZERO` and the standalone module `fbltrimzero`. Every name and path below is
> the one in use at the time and was deliberately left untouched. Current state:
> the collation is `ID_ZPAD_CI` (`WIN1252_ID_ZPAD_CI`, `ISO8859_1_ID_ZPAD_CI`), the
> driver is `src/intl/lc_id_zpad_ci.cpp`, the standalone module is `lrsintl`, and the
> live reference is `doc/README.id_zpad_ci.md`.

**Data:** 2026-08-02
**Branch:** `srs/ltrim-zero-numeric-order`, criada de `feature/ltrim-zero-collation-v5`
**Módulo:** `src/intl/lc_ltrim_zero.cpp`

## 1. Problema

A collation `LTRIM_ZERO` normaliza tirando zeros e espaços à esquerda e comparando sem
diferenciar caixa, e depois ordena o resultado da esquerda para a direita. Isso deixa a
ordem alfabética, não numérica:

```
ORDER BY V  ->  000, 10, 0001A34, 0000009, 9, A
```

`0001A34` vem antes de `9`, porque `'1' < '9'`.

O efeito prático não é só de relatório. Onde os valores têm quantidade de dígitos
diferente na mesma coluna, a ordem alfabética faz `BETWEEN` devolver linha errada.
Medido em banco de teste:

```sql
SELECT * FROM TD WHERE V BETWEEN '12345678000000' AND '12345678999999';
-- hoje:    12345678000199, 12345678009999, 12345678901
-- numérico: 12345678000199, 12345678009999
```

`12345678901` entra porque, sendo mais curto, é "menor" que o limite superior na
comparação caractere a caractere. Esse caso é real na base do cliente: a coluna do
domínio `TDR_CNPJ` guarda CNPJ de 14 dígitos e CPF de 11 na mesma coluna, e a busca
por raiz de CNPJ é feita justamente com `BETWEEN <raiz>00000 AND <raiz>99999`.

## 2. Decisão

Mudar a ordem da collation existente, no lugar, sem nome novo. A alternativa de criar
uma collation nova ao lado foi descartada pelo usuário: exigiria trocar a collation do
domínio em cada banco, e `ALTER DOMAIN ... TYPE ... COLLATE` não é aceito pelo parser
(verificado: `Token unknown - COLLATE`), o que deixaria só o caminho de restore
completo.

## 3. Comportamento

Normalização: inalterada. Espaços à direita saem quando a collation é `PAD SPACE`,
zeros e espaços à esquerda saem sempre, comparação em caixa alta ASCII.

Ordem: **tamanho do normalizado primeiro; empatou, byte a byte sobre a forma
normalizada.** Consequências:

- `9` antes de `0001A34`, que é o objetivo.
- Dentro do mesmo tamanho, dígito antes de letra (ordem ASCII: `'9' = 0x39 < 'A' = 0x41`).
- Sem diferenciar caixa: `a` e `A` continuam iguais, como hoje.
- Normalizado vazio (`'000'`, `'   '`, `''`) tem tamanho 0 e vem primeiro.

Igualdade não muda: `'000123' = '123'` continua verdadeiro, e a classe de equivalência
segue sendo a forma normalizada.

O tamanho que manda na ordem é o **da forma normalizada**, não o do valor gravado. É o
que faz `0000009` e `9` caírem na mesma posição:

| valor gravado | normalizado | tamanho | posição |
|---|---|---|---|
| `000` | (vazio) | 0 | 1ª |
| `9` | `9` | 1 | 2ª, empatado |
| `0000009` | `9` | 1 | 2ª, empatado |
| `A` | `A` | 1 | 4ª |
| `10` | `10` | 2 | 5ª |
| `0001A34` | `1A34` | 4 | 6ª |
| `12345678901` | `12345678901` | 11 | 7ª |
| `12345678000199` | `12345678000199` | 14 | 8ª |

Valores que só diferem nos zeros à esquerda continuam sendo iguais, e não apenas
vizinhos: a ordem entre eles é indiferente, e num índice `UNIQUE` colidem, exatamente
como já colidem hoje.

## 4. Chave de índice

```
[2 bytes: tamanho do normalizado, big-endian][bytes normalizados em caixa alta]
```

Big-endian porque a ordem byte a byte da chave precisa ser a mesma do `compare()`.
`texttype_fn_key_length` passa a devolver `len + 2`.

O tamanho cabe em 2 bytes: `string_to_key` recebe `USHORT` e o normalizado nunca cresce.

Verificado que a ordem da chave bate com a do `compare()` também no caminho descendente.
Como quase toda chave nova começa com `0x00` (byte alto do tamanho), o engine passa a
inserir o byte `desc_end_value_prefix` em praticamente toda chave descendente antes de
complementar (`btr.cpp:2929-2934`, `BTR_complement_key` em `btr.cpp:841-859`). A
monotonia se mantém, e a distinção entre NULL e vazio também.

### 4.1 Consumo de espaço de chave

A chave cresce 2 bytes por segmento, mais 1 byte em índice descendente, e em índice
composto os stuff bytes amplificam isso em torno de 25% (`btr.cpp:1727`). O limite é
`page_size / 4` (`Database.h:652-655`).

Consequência: índice que hoje vive perto do limite pode passar a estourar, e aí a falha
aparece na criação/rebuild (`idx.cpp:879-883`), no `INSERT` (`btr.cpp:664-665`) e na
consulta (`btr.cpp:2019`), com `isc_keytoobig`. Levantar, antes da troca, os índices
cujo comprimento declarado está próximo de `page_size / 4` faz parte do trabalho. No
`SCHERER_001`, com página de 16384, o limite é 4096 bytes e as colunas do domínio têm
20 bytes, então a folga é grande; a checagem existe para os outros bancos.

Há ainda o teto de `MAX_KEY = 8192` (`constants.h:195`) na chave de sort
(`intl.cpp:1013-1014`): acima disso a chave é truncada e o retorno não é checado por
`SortedStream.cpp:265`. `MAX_KEY` é constante de compilação, então esse limite não depende
de tamanho de página: verificado idêntico em página de 8192 e de 32768. Já é assim hoje, mas
os 2 bytes não afetam todo consumidor da mesma forma, e a truncagem depende da **largura
declarada da coluna**, não só do tamanho normalizado de um valor: `INTL_key_length`
dimensiona o espaço de chave de sort a partir do comprimento bruto do campo, igual para toda
linha da coluna. Tamanho normalizado 8191 é necessário para a truncagem ser possível (abaixo
disso, nenhuma largura de coluna dispara), mas não suficiente: numa coluna `VARCHAR(12000)`,
`dstLen` é 12000 para toda linha, e a truncagem só começa em tamanho normalizado 11999
(verificado: 11998 não colide em `DISTINCT`, 11999 colide), não em 8191.

`GROUP BY` revalida fronteira de grupo com compare real de valor (`AggregatedStream.cpp:308-
345`, `lookForChange`, `MOV_compare` na linha 345), então continua correto mesmo quando a
chave de sort trunca, de forma incondicional, porque valor igual sempre produz chave igual
e valor diferente é distinguido pelo compare, não pela chave. `DISTINCT` decide a partir da
chave truncada, mas não por `SortedStream::compareKeys` - essa função só tem um chamador em
toda a árvore, merge join (`MergeJoin.cpp:273`), sem relação com `DISTINCT`. O caminho real:
quando `FLAG_PROJECT` está ligado, `SortedStream::init` (`SortedStream.cpp:190`) passa
`RecordSource::rejectDuplicate` (`RecordSource.h:104`, devolve `true` incondicionalmente)
como callback de duplicata do sort, disparado por `DO_32_COMPARE` sobre a chave crua
(`sort.cpp:1301-1312`) sempre que duas chaves adjacentes comparam iguais byte a byte, sem
nenhuma revalidação de valor - nem o retorno de `FLAG_KEY_VARY` do CORE-4909 que
`compareKeys` tem (`SortedStream.cpp:286-317`). Por isso dois valores diferentes que colidam
na chave truncada se fundem em `DISTINCT`, verificado por execução, não só por leitura de
código (asserts 6b.3/6b.3b/6b.6 em `test_ltrim_zero.sql`).

Índice de verdade nunca alcança esse regime, e a margem é folgada. `CREATE INDEX` calcula
`key_length = ROUNDUP(INTL_key_length(len) + 1, 8)` (o `+1` é o byte indicador de NULL,
`idx.cpp:876-877`) e recusa quando isso é maior ou igual a `page_size / 4` (`idx.cpp:879`,
`Database.h:654`). No maior tamanho de página (32768, teto 8192), verificado: `VARCHAR(8181)`
cria, `VARCHAR(8182)` falha com "key size exceeds implementation restriction" (a checagem
real de `idx.cpp`), e `VARCHAR(8190)` falha antes ainda, numa checagem mais grosseira de
`MAX_KEY` no DSQL (`DdlNodes.epp`) com "key size too big for index". A coluna mais larga
ainda indexável é `VARCHAR(8181)`, 10 bytes inteiros abaixo de onde a truncagem de chave de
sort começaria. Só chave de sort (`ORDER BY`, `GROUP BY`, `DISTINCT`), que não tem teto de
`page_size / 4`, alcança esse regime. A menor coluna capaz de chegar lá é bem mais larga que
os 20 bytes do domínio `TDR_CNPJ`; a base do cliente não é alcançada.

## 5. STARTING WITH

Com a chave prefixada pelo tamanho, a chave de um prefixo deixa de ser prefixo da chave
do valor inteiro. O otimizador não pergunta: em `Retrieval.cpp:883-889` ele marca
`usePartialKey` sempre que o segmento é `segmentScanStarting`, `validateStarts`
(`Retrieval.cpp:2415-2494`) aceita qualquer `idx_itype >= idx_first_intl_string` sem
consultar a collation, e não existe flag de texttype para declarar que a collation não
sabe fazer chave parcial (`intlobj_new.h:126-139`). Patch no engine também não serve:
a produção troca só o `fbltrimzero.dll` sobre engine estoque.

**Decisão: `string_to_key` devolve 0, chave vazia, quando `key_type` é
`INTL_KEY_PARTIAL`.** O resultado continua correto, ao custo de varredura. A cadeia,
toda verificada no engine:

- `BTR_make_key` marca `key_empty` antes de comprimir (`btr.cpp:1900`), e comprimir
  comprimento 0 preserva o flag (`btr.cpp:2939-2946`);
- com scan fuzzy e chave vazia, a chave de busca fica com comprimento 0
  (`btr.cpp:1904-1908`; no caso composto, `btr.cpp:2005-2016`, os segmentos de igualdade
  anteriores continuam delimitando a faixa);
- chave "starting" vazia não encerra a varredura (`btr.cpp:6893` e `6939`): vira
  varredura completa do índice, sem perder linha;
- o `blr_starting` é reavaliado sobre o registro depois do bitmap, porque
  `CONJUNCT_MATCHED` não tira o boolean do filtro (`Optimizer.cpp:3008-3035`, com o
  `FilteredStream` montado em `3063`). O filtro só remove linha, então varredura ampla
  demais não gera falso positivo.

É o mesmo caminho de que a collation já depende hoje quando o prefixo é só de zeros
(`lc_ltrim_zero.cpp:162-167`).

O custo é de plano, não de resultado: `Retrieval.cpp:990` aplica
`REDUCE_SELECTIVITY_FACTOR_STARTING` achando que a faixa é seletiva, então o otimizador
subestima o custo de um scan que agora é completo.

**Alternativa descartada:** devolver `INTL_BAD_KEY_LENGTH`. O retorno não é conferido
por `INTL_string_to_key` (`intl.cpp:1245-1249`) nem por `btr.cpp:2882`, e
`btr.cpp:2926` trunca `(USHORT) -1` para o tamanho máximo de chave. O engine então monta
uma chave com memória não inicializada do buffer. Às vezes isso estoura o limite e vira
erro, às vezes o strip de bytes nulos (`btr.cpp:2948-2954`) encolhe a chave e o
resultado sai errado em silêncio. Não determinístico, e o pior dos dois mundos.

`INTL_KEY_MULTI_STARTING` não ajuda: cobrir "starts with" no formato prefixado exigiria
um entry por comprimento possível. `INTL_KEY_UNIQUE` e `INTL_KEY_SORT` recebem a chave
prefixada normal. Como a collation não declara `TEXTTYPE_SEPARATE_UNIQUE`, chave parcial
só é pedida em `STARTING WITH`: igualdade e `BETWEEN` usam `INTL_KEY_UNIQUE`/`SORT`
(`btr.cpp:1796-1797`), confirmado também para a checagem de FK (`idx.cpp:1919`, `1942`).

`LIKE 'x%'` é convertido em `blr_starting` pelo otimizador (`Optimizer.cpp:1279-1296`,
com `optimizeLikeSimilar` em `3303`) e cai na mesma regra, mantendo o `LIKE` original
como filtro residual.

### 5.1 Efeito colateral registrado

Com o `SORT`/`UNIQUE` nunca mais devolvendo comprimento 0, o flag `key_empty` deixa de
ser marcado para normalizado vazio (`btr.cpp:2921-2924`). Os consumidores desse flag
estão todos no caminho fuzzy e de varredura de NULL (`btr.cpp:1904`, `2008`, `6798`,
`6893`), e a decisão da seção 5 devolve chave vazia justamente onde ele importa.

## 6. Compatibilidade e janela de troca

**Todo índice existente sobre coluna com essa collation guarda chave no formato
antigo.** Depois de trocar o módulo, esses índices descrevem a ordem velha e devolvem
resultado errado até serem reconstruídos. Isso vale para:

- `SCHERER_001.FDB`: 421 segmentos de índice sobre as 368 colunas do domínio `TDR_CNPJ`.
- Índices de expressão cujo resultado carregue a collation, que **não** aparecem num
  inventário por `RDB$INDEX_SEGMENTS` e precisam entrar na lista à parte
  (`btr.cpp:2059`).
- Qualquer outro banco do servidor que use `ISO8859_1_LTRIM_ZERO` ou `WIN1252_LTRIM_ZERO`.
  São as duas únicas collations registradas, tanto no `fbintl` (`src/intl/ld.cpp:386,445`)
  quanto no módulo standalone (`fbltrimzero.conf`). O nome local pode ser outro: quem
  manda é o `RDB$COLLATIONS.RDB$BASE_COLLATION_NAME`, que guarda o nome do
  `FROM EXTERNAL` (`DdlNodes.epp:3977`).

**Nenhuma escrita pode ocorrer entre trocar o dll e terminar o rebuild.** Não é só
consulta: a checagem de chave estrangeira monta chave nova e procura no índice do
parceiro, que ainda está no formato antigo (`idx.cpp:1942-1971`), então `INSERT` e
`UPDATE` nesse intervalo furam integridade referencial e unicidade sem reclamar.

**`ALTER INDEX ... INACTIVE` não serve para índice de constraint.** O trigger de sistema
recusa com "Cannot deactivate index used by a PRIMARY/UNIQUE constraint"
(`trig.h:1377-1378`, `ini.epp:189`). Com 421 segmentos, boa parte é de PK, UNIQUE ou FK,
então o caminho realista é **backup e restore**, que reconstrói tudo, com
`ALTER INDEX INACTIVE/ACTIVE` reservado para os índices comuns caso se queira uma janela
menor. Não há decisão a medir: um rebuild seletivo deixaria os índices de constraint no
formato antigo enquanto o dll já fala o novo, e esse é o estado de resultado errado, não o
estado lento. O caminho é backup e restore.

Entre a troca do dll e o fim do rebuild, `gfix -v` reporta corrupção de índice. É
esperado, e não indica problema novo.

## 7. Build do módulo

A produção carrega `fbltrimzero.dll`, um módulo separado de 15 KB, e o
`fbintl.conf` instalado já traz `#include $(root)/intl/fbltrimzero.conf`. Esse dll foi
gerado há tempos a partir deste repositório, com a versão antiga do fonte, sem receita
guardada. O repositório `D:\GitHub\ltrim-zero-module` tem `Makefile` e `ld_min.cpp`,
voltados para Linux/docker.

Escopo desta mudança inclui **estabelecer e documentar uma receita MSVC** para gerar o
`fbltrimzero.dll` no Windows a partir de `src/intl/lc_ltrim_zero.cpp` mais a camada
mínima de exportação. Assim a troca em produção é só substituir esse arquivo, sem
encostar no `fbintl.dll` oficial do Firebird.

O `fbintl.dll` do nosso build também compila a collation embutida, e continua servindo
para testar sem instalar nada.

## 8. Testes

Unitários no harness que já existe para a collation
(`src/jrd/tests/LtrimZeroCollationTest.cpp`) mais checagem em SQL:

1. `9` antes de `0001A34` em `ORDER BY`.
2. Dentro do mesmo tamanho: `9` antes de `A`; `a` e `A` na mesma posição.
3. Normalizado vazio primeiro.
4. Igualdade preservada: `'000123' = '123'`, `'0000009' = 9`.
5. `BETWEEN '12345678000000' AND '12345678999999'` não traz o valor de 11 dígitos.
6. Índice contra varredura (`col` versus `col || ''`) com o mesmo resultado, em
   igualdade, `BETWEEN` e `ORDER BY`, sobre volume que force uso de índice.
7. `STARTING WITH` sobre coluna indexada devolve **o mesmo resultado** da varredura,
   inclusive com prefixo só de zeros e com prefixo que não existe.
8. Chave e `compare()` concordando: para cada par do conjunto de teste, o sinal de
   `compare()` bate com a comparação byte a byte das chaves. Incluir índice descendente
   e índice composto no conjunto.

O teste 8 é o que segura a invariante que sustenta tudo: índice e comparação têm que
contar a mesma história.

## 9. Riscos

- **Desempenho de `STARTING WITH` e `LIKE 'x%'`** sobre coluna do domínio: o resultado
  continua certo, mas a consulta passa a varrer o índice inteiro, e o otimizador ainda
  acha que a faixa é seletiva. No código armazenado do banco do cliente não há nenhum
  `STARTING WITH` (0 procedures, 0 triggers, 0 views) e só 4 procedures usam `LIKE`, mas
  o SQL que a aplicação monta não dá para inspecionar daqui.
- **`BETWEEN` muda de significado além do caso CNPJ.** `BETWEEN '9' AND '11'` passa a
  incluir todo valor de tamanho 1 maior que `9`, inclusive letras, antes de chegar em
  `10`. Onde a coluna mistura número e texto, vale revisar as faixas.
- **Índice perto do limite de chave** pode passar a estourar com os 2 bytes a mais
  (seção 4.1).
- **Rebuild em base grande é demorado e precisa de janela sem escrita**, com a ressalva
  das constraints (seção 6).
- Ordem de relatório muda para quem já dependia da ordem alfabética atual.

# Revisão: `src/intl/lc_ltrim_zero.cpp` (branch `feature/ltrim-zero-collation-v5`)

> **Status: corrigido e verificado no Windows e no Linux, em Release e em DEV_BUILD.**
>
> - Driver reescrito conforme P3 + C2 + A3 + A4.
> - `test_ltrim_zero.sql`: **55 asserts, 55 PASS, 0 FAIL**.
> - `src/jrd/tests/LtrimZeroCollationTest.cpp`: **7 casos, todos passando**, incluindo
>   8 attachments concorrentes inserindo 50000 linhas, validação online do banco
>   (`0 errors, 0 warnings, 0 fixed`), índice DESCENDING com chave vazia, índice
>   composto, PK/FK e round-trip de gbak.
> - **Windows x64 Release** (MSVC): as duas suítes verdes.
> - **Linux x64** (Ubuntu 22.04, clang, Docker): as duas suítes verdes em Release
>   **e** com `--enable-developer`. `make run_tests` completo passa nas duas.
>   O `fb_assert` de `src/jrd/intl.cpp:398-399` fica ativo na passada developer
>   (376 linhas compiladas com `-DDEV_BUILD`) e **não dispara**, o que fecha C2.
>   Harness em `doc/ltrim_zero_docker/`, instruções na seção 6.3 do README.
> - O build POSIX pega `lc_ltrim_zero.cpp` pelo glob, confirmado por execução:
>   `temp/Release/intl/lc_ltrim_zero.o` é gerado sem nenhuma alteração de build file,
>   e `LCLTRIMZERO_init` fica como símbolo **local** em `libfbintl.so`.
> - Um item da revisão original (C3) **estava errado**; o teste desmentiu e o texto foi corrigido.
>
> **Não verificado:** carga de produção real. 8 threads e 50000 linhas num container
> provam correção, não capacidade.

Alvo: Windows + Linux, OLTP de carga altíssima.
Veredito: **não subir como está.** Não há estouro de buffer, mas há 4 defeitos semânticos independentes que produzem resultado errado silencioso, 1 violação do contrato "callback não pode lançar exceção" e 1 defeito grave de escalabilidade.

O que está certo: o driver é *stateless* (sem estado mutável compartilhado), logo é reentrante e thread-safe de verdade. E a integração de build está completa - POSIX (`builds/posix/make.shared.variables:4-7,137`) e CMake (`src/CMakeLists.txt:518`) fazem glob de `src/intl/*.cpp`; `fbintl.vers` não precisa exportar `LCLTRIMZERO_init` (só é referenciado dentro do próprio `.so`, via `src/intl/ld.cpp:212,386,445`).

---

## OPERACIONAL - antes de tudo

### O0. Corrigir os bugs muda os bytes da chave de índice. Todo índice existente precisa ser reconstruído.

Nenhuma correção abaixo (O1, C1, C4, M1) preserva o formato de chave atual. Trocar `fbintl.dll` / `libfbintl.so` num banco que já tem índice sobre coluna `LTRIM_ZERO` = linhas silenciosamente não encontradas, **sem nenhum erro**. Índice não é validado contra a versão da collation.

Caminho seguro: backup/restore (gbak), ou `ALTER INDEX ... INACTIVE` + `ACTIVE` em todos os índices afetados, depois de trocar a lib. Em produção OLTP isso é a decisão de rollout mais importante desta feature.

### O1. `src/CMakeLists.txt:518` - glob sem `CONFIGURE_DEPENDS`

```cmake
file(GLOB intl_src "intl/*.cpp" "intl/*.h")
```

Avaliado só em configure-time. Build tree CMake já existente continua gerando `fbintl` **sem** `lc_ltrim_zero.o`, em silêncio. Rodar `cmake` de novo, ou adicionar `CONFIGURE_DEPENDS`. É o sintoma "minha collation não existe" mais provável no Linux.

---

## CRÍTICOS

### C1. PAD SPACE não implementado. `WHERE c = 'A'` nunca casa em coluna CHAR.

O motor **não** remove espaços à direita quando o driver fornece `fn_compare` / `fn_string_to_key`. `TextType::compare` (`src/common/TextType.cpp:251-252`) delega direto ao driver; o código de strip em `TextType.cpp:283-302` e `215-226` só roda no fallback de ponteiro NULL. O comentário do header é explícito (`src/common/intlobj_new.h:158-160`): *"this is the job of string_to_key and compare routines"*. Todos os drivers in-tree fazem isso sozinhos - `lc_narrow.cpp:576-588` e `197-204`, `lc_ascii.cpp:531-538`, `intl_builtin.cpp:482-492`.

`lc_ltrim_zero.cpp:271` seta `texttype_pad_option` e **nunca mais olha para ele**. `normalize_string` (linhas 62-93) só corta à esquerda.

Repro exato: coluna `CHAR(5) COLLATE WIN1252_LTRIM_ZERO` guardando `'A'` -> bytes `"A    "`, len 5. Literal `'A'` tem len 1.
`N("A    ") = "A    "` (5), `N("A") = "A"` (1). `texttype_fn_compare:156-157`: `norm_len1 > norm_len2` -> retorna 1.
**Nenhuma igualdade em coluna CHAR funciona.** Em VARCHAR, `'A ' <> 'A'` mesmo com a collation declarada PAD SPACE.

Correção: cortar `' '` à direita nos dois - `fn_compare` e `fn_str_to_key`. Dada a semântica (espaço à esquerda já é insignificante), cortar **incondicionalmente**, não só quando `pad_option` estiver ligado; senão CHAR vs VARCHAR vs literal fica incoerente sob NO PAD.

### C2. `canonical_width = 1` com `fn_canonical = NULL` - **este é o item que quebra no Linux**

`lc_ltrim_zero.cpp:272` seta `texttype_canonical_width = 1`, mas `texttype_fn_canonical` nunca é atribuído.

`src/jrd/intl.cpp:398-399`:
```cpp
fb_assert((tt->texttype_canonical_width == 0 && tt->texttype_fn_canonical == NULL) ||
          (tt->texttype_canonical_width != 0 && tt->texttype_fn_canonical != NULL));
```

DEV_BUILD é o build padrão de desenvolvedor no Linux -> **aborta na primeira carga da collation**. A parte de build está limpa (globs), então este é o único item Linux-específico de fato.

Em Release o assert some, `TEXTTYPE_DIRECT_MATCH` não é ligado (`intl.cpp:401-411` pulado) e `TextType::canonical` cai no `memcpy` identidade (`TextType.cpp:385`).

Repro (Release): linha `'ABC'`.
- `WHERE col = 'abc'` -> retorna (fn_compare faz upper).
- `WHERE col LIKE 'abc'` -> não retorna (canonical são os bytes crus).
- `WHERE col CONTAINING 'abc'` -> não retorna. CONTAINING é documentado como sempre case-insensitive no Firebird.
- `SIMILAR TO` e `STARTING WITH` viram case-sensitive.

Correção: implementar `texttype_fn_canonical` gravando `ascii_toupper(src[i])` por byte, retornando `srcLen` (contagem de **caracteres**, exatamente `canonical_width=1` byte por caractere; buffer dimensionado em `src/jrd/intl_classes.h:105-117`).

Limite conhecido: um canonical por-caractere **não consegue** expressar remoção de zeros à esquerda. Logo `LIKE`/`CONTAINING` nunca vão concordar 100% com `=` nesta collation. Se isso for inaceitável, índice por expressão ou coluna normalizada persistida dá a mesma semântica sem esse buraco - decisão de vocês, não estou redesenhando a feature.

### C3. `STARTING WITH` por índice perdia linha - **CORRIGIDO, e o diagnóstico original estava exagerado**

O bug real era o ramo "guarda o último caractere" (A1): com ele, `N("00") = "0"` enquanto `N("00A") = "A"`, então a chave parcial `"0"` não cobria a linha e ela **sumia** no plano por índice.

Removido o ramo, `N("00") = ""` (chave vazia). O motor então zera `key_length` para busca fuzzy (`src/jrd/btr.cpp:1904-1908`) e varre o índice inteiro - **falso negativo impossível**. Nenhum tratamento especial de `key_type` é necessário: `N(prefixo)` sempre é prefixo em bytes de `N(valor)`, porque o corte só acontece no início da string.

> **Atualização (numeric order, `docs/superpowers/specs/2026-08-02-ltrim-zero-numeric-order-design.md` seção 5):** a premissa acima - "`N(prefixo)` sempre é prefixo em bytes de `N(valor)`" - deixou de valer quando a chave passou a levar um prefixo de 2 bytes com o tamanho do normalizado (seção 4 da spec citada acima, "Chave de índice"). A chave de `N(prefixo)` tem tamanho diferente do de `N(valor)`, então o prefixo de tamanho por si só já quebra a relação de prefixo em bytes, mesmo quando os bytes normalizados batem. A correção adotada foi tornar `INTL_KEY_PARTIAL` **sempre** especial: `string_to_key` devolve chave vazia para **todo** `STARTING WITH`, não só para prefixo inteiramente removível. O resultado continua correto (mesmo mecanismo de varredura completa descrito aqui), só que agora é o caminho de **todo** prefixo, não só do zero-only.

**Onde eu errei:** afirmei que índice e scan natural devolveriam conjuntos diferentes porque o predicado casado pelo índice não é reavaliado. **Não é o que acontece.** Medido:

| query | `PLAN NATURAL` | `PLAN INDEX` |
|---|---|---|
| `STARTING WITH '00'` | 5 | 5 |
| `STARTING WITH '0000'` | 4 | 4 |
| `STARTING WITH 'A'` | 3 | 3 |

O motor reavalia o `STARTING WITH` depois do fetch, então o range largo do índice é filtrado de volta para a semântica canonical. Os dois planos concordam. Coberto pelos asserts 8.6, 8.6b, 8.6c e 8.7.

### C4. `MAX_SAFE_STRING = 32000` torna a comparação intransitiva

`TextType.cpp:249-252`: o motor declara `INTL_BOOL error`, passa o endereço ao driver e **nunca lê**. Portanto todo `*error_flag = 1; return 0;` (linhas 108-109, 113-115) é interpretado como **"as strings são iguais"**.

E 32000 está abaixo dos limites legais do Firebird 5: `MAX_COLUMN_SIZE = 32767`, `MAX_VARY_COLUMN_SIZE = 32765` (`src/jrd/constants.h:55-56`); expressões intermediárias vão até `MAX_STR_SIZE = 65535` (`constants.h:58`). `VARCHAR(32100) CHARACTER SET WIN1252` é coluna legal.

Repro de intransitividade: `X` = 32500 x `'0'` + `'A'`; `A` = `'A'`; `B` = `'B'`.
`compare(A,X) = 0`, `compare(X,B) = 0`, `compare(A,B) = -1`. X é igual aos dois, mas A < B.
Comparador intransitivo alimenta merge join e nested loop -> resultado errado silencioso. `WHERE longcol = 'x'` casa **toda** linha com mais de 32000 bytes.

VERIFICADO após a correção (asserts 6.x e 6b.x): valores de 32600 bytes comparam e agrupam certo, e num teste de controle com chave normalizada de 12000 bytes (acima de `MAX_KEY` = 8192) a coluna `LTRIM_ZERO` produz exatamente os mesmos 2 grupos que a mesma coluna com a collation padrão. Não há colapso nem regressão em relação ao resto do motor.

> **Atualização (numeric order):** isso era verdade para o formato de chave da época desta revisão (chave = `N(x)`, sem prefixo). Com o prefixo de tamanho de 2 bytes (M3 abaixo), a truncagem em `MAX_KEY` (constante de compilação, `constants.h:195`, sem depender de tamanho de página) passa a cortar 2 bytes a mais de cauda. Isso depende da largura declarada da coluna, não só do tamanho normalizado do valor: numa coluna `VARCHAR(12000)` como a deste teste, a truncagem só começa em tamanho normalizado 11999 (verificado: 11998 não colide, 11999 colide), não em 8191 - 8191 é o piso necessário para a truncagem ser possível em qualquer coluna, não o gatilho nesta. Isso alcança `DISTINCT`, mas não por `SortedStream::compareKeys` (essa função só é chamada por merge join, `MergeJoin.cpp:273`, sem relação com `DISTINCT`): o caminho real é `RecordSource::rejectDuplicate` (`RecordSource.h:104`, devolve `true` incondicionalmente), passado como callback de duplicata pelo sort quando `FLAG_PROJECT` está ligado (`SortedStream.cpp:190`) e disparado por `DO_32_COMPARE` sobre a chave crua (`sort.cpp:1301-1312`) sem nenhuma revalidação de valor - nem o retorno de `FLAG_KEY_VARY` do CORE-4909 que `compareKeys` tem (`SortedStream.cpp:286-317`). `GROUP BY` continua batendo com a collation padrão, sem exceção, porque revalida com compare real de valor (`AggregatedStream.cpp:308-345`, `MOV_compare` na linha 345). Índice de verdade nunca alcança esse regime, com folga: a coluna mais larga ainda indexável é `VARCHAR(8181)` no maior tamanho de página (ver M3). Asserts 6b.3/6b.3b/6b.6 em `test_ltrim_zero.sql` cobrem os fatos.

No lado da chave, o dano era em sort/agrupamento, **não** no índice: `INTL_key_length` clampa em `MAX_KEY` = 8192 (`src/jrd/intl.cpp:982-1020`), então um valor acima de 32000 bytes nem chega a `string_to_key` pelo caminho do btree. Mas `str_to_key` também é chamado por SortedStream/HashJoin/AggNodes, para colunas **não** indexadas. Ali `SortedStream.cpp:265` ignora o retorno, deixando a chave de sort toda zerada (buffer pré-zerado em `SortedStream.cpp:208`). Resultado: **todos os valores acima de 32000 bytes colapsam num único grupo em ORDER BY / GROUP BY**.

Correção: remover o cap. Com o algoritmo sem alocação (P1) não sobra modo de falha - que é o único estado aceitável, já que o motor ignora o canal de erro.

---

## ALTOS

### A1. O ramo "guarda o último caractere" (linhas 77-84) quebra a classe de equivalência em três

Bytes normalizados, concretamente:

| entrada | `N(entrada)` |
|---|---|
| `"0"`, `" 0"`, `"00"`, `"000"` | `"0"` (0x30) |
| `" "`, `"0 "`, `"00 "`, `" 0 "` | `" "` (0x20) |
| `""` | vazio (já correto hoje - linha 171 retorna antes de `normalize_string` rodar) |

Todas são "só zeros e espaços" e pela intenção da collation deveriam ser iguais. A classe é partida **por qual caractere calhou de ser o último fisicamente**.

- `'0' = '00'` VERDADEIRO, mas `'0' = '0 '` FALSO e `'00' = '00 '` FALSO. Um espaço à direita inverte a igualdade.
- `' 0' = '0'` VERDADEIRO e `'0 ' = ' '` VERDADEIRO, mas `' 0' = '0 '` FALSO.
- Combinado com C1: `CHAR(2)` guardando `'0'` é `"0 "`, `N = " "`; literal `'0'` dá `"0"` -> `WHERE c = '0'` erra. `CHAR(n)` guardando `''` é tudo espaço, `N = " "`; literal `''` dá `""` -> `WHERE c = ''` erra.

Ponto importante para não confundir: a relação **é** transitiva hoje. Igualdade é exatamente `N(a) == N(b)` para uma função pura `N`, então é equivalência de verdade, e compare/chave concordam (a chave **é** `N`, e a ordem memcmp com "prefixo menor primeiro" de `btr.cpp:4662-4672` bate com a regra de `fn_compare`). O defeito não é intransitividade - **as classes é que estão erradas**. A intransitividade só entra por C4.

> **Atualização (numeric order):** "a chave **é** `N`" era verdade na época desta revisão e não é mais. A chave agora é `[2 bytes de tamanho, big-endian][N(x)]` (ver README seção 2 e spec seção 4), por causa da ordenação numérica. A concordância entre compare e chave que este parágrafo descreve continua valendo - duas entradas na mesma classe de equivalência têm `N` igual, logo tamanho igual, logo chave igual -, mas com uma exceção acima de `MAX_KEY` (ver M3 e C4): `DISTINCT` pode divergir de `=` ali porque decide a partir da chave truncada, não do valor.

Corolário: **não** setar `TEXTTYPE_SEPARATE_UNIQUE`. Não é necessário. Mas, para essa propriedade sobreviver a edições futuras, `fn_compare` e `fn_str_to_key` devem compartilhar um único caminho de normalização, não duas cópias da lógica.

Correção: deletar o ramo. Com o strip à direita (C1), tudo que for inteiramente removível normaliza para string vazia: `"000"`, `"  "`, `" 0 "`, `""` todos iguais, todos com chave vazia. Só o caso "removível e não-vazio" muda de comportamento - a string vazia já é tratada certo hoje.

Verificado que isso é seguro: o motor distingue chave vazia de NULL por campos **separados** - `key_nulls` para NULL (`btr.cpp:594`, `1898`) e a flag `key_empty` para vazio (`btr.h:149`, setada em `btr.cpp:1900`, limpa em `btr.cpp:2924` quando `length != 0`). O caminho de comprimento 0 já existe e funciona hoje (`btr.cpp:2939-2946` grava um byte de pad e mantém `key_empty`).

### A2. Contrato "callback não lança" violado, mais um vazamento

`FB_NEW_POOL(*getDefaultMemoryPool()) UCHAR[len]` (linhas 122-123, 178) **lança `Firebird::BadAlloc`, nunca retorna NULL**.

- Os blocos `if (!norm1 || !norm2)` (125-132, 180-181) são código morto e dão falsa sensação de tratamento de erro.
- Se a alocação da linha 122 der certo (heap, `len1 > 1024`) e a 123 lançar, **`norm1` vaza** - não há RAII.
- Não existe try/catch entre o motor e os ponteiros de função: `TextType.cpp:175-388` chama todos crus. A exceção sobe para o código de btree/sort. Durante geração de chave de índice com páginas travadas, isso vira bugcheck sob pressão de memória. Todo driver in-tree captura internamente e devolve `INTL_BAD_KEY_LENGTH` / `INTL_BAD_STR_LENGTH` (`lc_ascii.cpp:83-86`, `IntlUtil.cpp:841-845`, `906-910`).
- Menor: `FB_NEW TextTypeImpl` no init (linha 265) também pode lançar através da fronteira do módulo, e o objeto é uma struct vazia. Deixar `texttype_impl = nullptr` e remover `fn_destroy` - o motor nunca toca nesse campo (verificado: nenhuma leitura fora dos próprios drivers).

### A3. `UPPER()` para de funcionar com acento - regressão visível em dados pt-BR

`texttype_fn_to_upper` / `to_lower` (linhas 216-245) substituem o mapeamento de caixa completo do charset por ASCII puro. Numa coluna WIN1252 com essa collation, `UPPER('ção')` deixa de virar `'ÇÃO'`.

Correção de uma linha: **não setar** `cache->texttype_fn_str_to_upper` nem `..._str_to_lower`. O fallback do motor (`TextType::str_to_upper` -> `IntlUtil::toUpper`, charset -> UTF-16 -> ICU -> charset) faz certo. Estritamente melhor que reimplementar.

Bônus: as versões atuais também ignoram que **origem e destino podem ser o mesmo ponteiro** (`intlobj_new.h:176-177`, caso real em `src/dsql/ExprNodes.cpp:10848`) - o loop forward atual sobrevive a isso por acaso, mas o fallback do motor já trata explicitamente.

---

## PERFORMANCE (eixo "carga altíssima")

### P1. `memset` de ~32 KB por chave de índice - maior ganho, deleção de 3 linhas

`btr.cpp:2880` seta `to.dsc_length = MIN(MAX_COLUMN_SIZE, MAX_KEY * 4)` = **32767**, independente do tamanho real da string. As linhas 198-199 então zeram `dstLen - norm_len` bytes:

```cpp
if (norm_len < dstLen)
    memset(dst + norm_len, 0, dstLen - norm_len);
```

Todo insert/update/delete/lookup em coluna indexada dessa collation grava ~32 KB de zeros. A 10k inserts indexados/s isso é ~320 MB/s de escrita inútil, e cada chave arrasa a L1d.

O padding não tem valor nenhum: o btr usa só o comprimento retornado e ainda tira zeros à direita (`btr.cpp:2948-2952`); `SortedStream` pré-zera o registro (`SortedStream.cpp:208`); `HashJoin` pré-zera o buffer (`HashJoin.cpp:651`); o driver builtin nunca faz padding (`intl_builtin.cpp:479-494`).

Sobre segurança: o `memset` está **dentro dos limites nos três caminhos verificados** (`VaryStr<32768>` em `btr.cpp:2816`, registro de sort pré-zerado, buffer de hash). Não é um estouro. Delete assim mesmo.

### P2. Alocação no pool global compartilhado, por chamada de compare

Qualquer compare com um dos lados acima de 1024 bytes faz 2 alocações + 2 frees no **pool default do processo** (`getDefaultMemoryPool()`, `alloc.h:291-295`). `MemPool::allocate` / `releaseBlock` pegam o mutex do pool (`alloc.cpp:2363`, `2447`, mais o lock do pool pai em `2469`). Merge join ou sort sobre 1M de linhas de ~2 KB = ~4M pares lock/unlock num mutex único do processo inteiro. Isso serializa as threads do SuperServer - exatamente o cenário de carga altíssima.

Além disso, todo compare copia as duas strings inteiras antes do `memcmp` - 2x de tráfego de memória contra os drivers narrow in-tree, que comparam por tabela byte a byte sem cópia.

### P3. Substituição: passe único, zero alocação

```cpp
// compare
const UCHAR* e1 = s1 + len1; while (e1 > s1 && e1[-1] == ' ') e1--;   // strip direita
const UCHAR* e2 = s2 + len2; while (e2 > s2 && e2[-1] == ' ') e2--;
while (s1 < e1 && (*s1 == '0' || *s1 == ' ')) s1++;                    // strip esquerda
while (s2 < e2 && (*s2 == '0' || *s2 == ' ')) s2++;
while (s1 < e1 && s2 < e2) {
    const UCHAR c1 = ascii_toupper(*s1++), c2 = ascii_toupper(*s2++);
    if (c1 != c2) return c1 < c2 ? -1 : 1;
}
return (s1 < e1) ? 1 : (s2 < e2) ? -1 : 0;

// str_to_key
const UCHAR* e = src + srcLen; while (e > src && e[-1] == ' ') e--;    // strip direita
const UCHAR* p = src;          while (p < e && (*p == '0' || *p == ' ')) p++;  // strip esquerda

// C3: prefixo inteiramente removível -> chave vazia, casa tudo, motor filtra depois
if (key_type == INTL_KEY_PARTIAL && p >= e)
    return 0;

USHORT n = 0;
while (p < e && n < dstLen) dst[n++] = ascii_toupper(*p++);
if (p < e) return INTL_BAD_KEY_LENGTH;   // não coube: falha explícita, não trunca
return n;                                 // sem memset do resto de dstLen
```

Sem buffer, sem alocação, sem cap de tamanho, sem caminho de exceção, O(n), e os bytes da chave continuam sendo exatamente `N(x)` - preserva a concordância compare/chave. Resolve C1, C3, C4, A1, A2, P1, P2 de uma vez. C2 continua sendo adição separada.

> **Atualização (numeric order):** o trecho de `str_to_key` acima é anterior à mudança de ordenação numérica e não reflete mais o driver de hoje em dois pontos. Primeiro, `key_type == INTL_KEY_PARTIAL` não checa mais `p >= e`: **qualquer** `STARTING WITH` devolve chave vazia agora, porque o prefixo de tamanho na chave (seção 4 da spec de numeric order, "Chave de índice") quebra a relação de prefixo em bytes para todo prefixo, não só para o inteiramente removível - ver a atualização na seção C3 acima. Segundo, a chave deixou de ser exatamente `N(x)`: é `[2 bytes de tamanho, big-endian][N(x)]`, e `texttype_fn_key_length` (M3 abaixo) devolve `len + 2`, não `len`.

---

### A4. `SIMILAR TO` ficava case-sensitive - achado só pelos testes

`SIMILAR TO` **não** passa por `texttype_fn_canonical`. Ele compila a expressão para RE2 e lê a insensibilidade de caixa direto dos atributos da collation: `src/jrd/Collation.cpp:132` e `:235`, `flags |= (textType->getAttributes() & TEXTTYPE_ATTR_CASE_INSENSITIVE) ? SimilarToFlag::CASE_INSENSITIVE : 0`.

O init rejeitava esse atributo, então `CREATE COLLATION ... CASE INSENSITIVE` falhava e `SIMILAR TO '%a%'` devolvia 2 linhas enquanto `LIKE '%a%'` devolvia 9.

Correção: aceitar `TEXTTYPE_ATTR_CASE_INSENSITIVE` no init (a collation já é sempre CI, o atributo só declara isso). `ACCENT INSENSITIVE` continua rejeitado - aceitá-lo faria `SIMILAR TO` dobrar acentos enquanto `=` não dobra. A DDL passa a ser:

```sql
CREATE COLLATION WIN1252_LTRIM_ZERO FOR WIN1252
    FROM EXTERNAL ('WIN1252_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE;
```

VERIFICADO: assert 8.3, `SIMILAR TO '%a%'` = 9 = `LIKE '%a%'` = `CONTAINING 'a'`.

---

## MENORES

### M1. Pegadinhas semânticas para documentar (ou rejeitar)

- **Ordem é por codepoint, não numérica**: `'10' < '9'` (0x31 < 0x39). Quem adota uma collation "tira zero à esquerda" costuma esperar ordem numérica. Compare e índice concordam entre si, ambos "errados" contra a expectativa.

  > **Atualização (numeric order):** esse item foi resolvido, no sentido oposto ao texto acima. A ordem passou a ser por tamanho do normalizado primeiro, byte a byte em segundo (`docs/superpowers/specs/2026-08-02-ltrim-zero-numeric-order-design.md` seção 3), então agora `'9' < '10'`, batendo com a expectativa numérica, não contrariando ela. `doc/README.ltrim_zero.md` seção 3 documenta a ordem nova. Mantido aqui como registro histórico do que motivou a mudança.
- Case-insensitive é só ASCII, registrada em WIN1252/ISO8859_1: `'é' <> 'É'` na comparação.
- `UNIQUE`/PK vão rejeitar `'0A'` depois de `'A'` como duplicata. É a semântica pedida, mas precisa estar escrito.
- O init rejeita tudo que não seja PAD SPACE (linha 261), então `CREATE COLLATION ... CASE INSENSITIVE` falha com erro confuso, mesmo a collation sendo case-insensitive.

### M2. `TEXTTYPE_ENTRY` -> `TEXTTYPE_ENTRY3` (linha 258)

`TEXTTYPE_ENTRY` (`ldcommon.h:49-53`) nomeia `cs` e `specific_attributes`, ambos não usados -> dois `-Wunused-parameter` no gcc/clang. `TEXTTYPE_ENTRY3` (`ldcommon.h:61-65`) é o macro certo. Cosmético.

### M3. `texttype_fn_key_length` está correto - **superado pela ordenação numérica**

Na época desta revisão, retornar `len` era certo: a chave era exatamente `N(x)`, sem crescer, e `INTL_key_length` (`src/jrd/intl.cpp:982-1020`) clampava em `[iLength, MAX_KEY]` de qualquer forma.

A ordenação numérica (`docs/superpowers/specs/2026-08-02-ltrim-zero-numeric-order-design.md` seção 4) muda essa conclusão: a chave passa a precisar de 2 bytes extras para o tamanho do normalizado, big-endian, na frente dos bytes normalizados, porque a ordem passou a ser por tamanho primeiro. `texttype_fn_key_length` agora devolve `len + 2`. Consequência operacional: o índice mais largo indexável encolhe 2 bytes (ver seção 4.1 da spec), e o mesmo clamp em `MAX_KEY` (constante de compilação, `constants.h:195`, o mesmo em qualquer tamanho de página) que antes só cortava a chave de sort agora corta 2 bytes a mais de cauda. Isso depende da largura declarada da coluna, não de um tamanho normalizado fixo: 8191 é o piso necessário para a truncagem ser possível, mas numa coluna larga o gatilho real fica bem acima disso (numa `VARCHAR(12000)`, começa em 11999). Isso atinge `DISTINCT`, através de `RecordSource::rejectDuplicate` (`RecordSource.h:104`, sempre `true`) disparado por `DO_32_COMPARE` sobre a chave crua quando o sort roda em modo `FLAG_PROJECT` (`SortedStream.cpp:190`, `sort.cpp:1301-1312`), sem nenhuma revalidação de valor - `SortedStream::compareKeys` não entra aqui, seu único chamador é merge join (`MergeJoin.cpp:273`). `GROUP BY` revalida com compare real de valor (`AggregatedStream.cpp:308-345`, `lookForChange`, `MOV_compare` na linha 345) e continua correto sempre. Índice de verdade nunca alcança esse regime, com folga: `CREATE INDEX` calcula `ROUNDUP(INTL_key_length(len) + 1, 8)` (`idx.cpp:876-877`) contra `page_size / 4` (`Database.h:654`, `idx.cpp:879`), e no maior tamanho de página (32768, teto 8192) a coluna mais larga ainda indexável é `VARCHAR(8181)` - verificado, não só calculado - 10 bytes abaixo de onde a truncagem de sort começaria (asserts 6b.3/6b.3b/6b.6 em `test_ltrim_zero.sql`).

### M4. Portabilidade Linux - limpa, fora de C2

`UCHAR` usado em tudo, `memcmp` é unsigned por definição, nada depende do sinal de `char` (relevante porque `char` é unsigned em Linux/ARM). O `TextTypeImpl` em namespace anônimo (linhas 37-44, commit `044d3c6763`) evita corretamente a colisão de ODR com o `TextTypeImpl` de escopo global dos drivers vizinhos - no Linux o linker teria fundido em silêncio. Commit certo.

### M5. Fora do escopo, mas vai quebrar o build POSIX

`src/burp/restore - Copia.epp` e `src/burp/restore - Copia (2).epp` estão untracked na árvore. Se forem commitados, `dirObjects,burp` (`make.shared.variables:107`) faz glob de `*.epp` e compila as duas cópias dentro do gbak -> falha de símbolo duplicado no link POSIX. Apagar.

---

## Ordem de execução sugerida

| # | Item | Status |
|---|---|---|
| 1 | P3 - reescrita passe único, zero alocação (resolve C1, C3, C4, A1, A2, P1, P2) | FEITO |
| 2 | C2 - adicionar `fn_canonical` | FEITO |
| 3 | A3 - remover `fn_str_to_upper` / `fn_str_to_lower` | FEITO |
| 4 | A4 - aceitar `TEXTTYPE_ATTR_CASE_INSENSITIVE` | FEITO |
| 5 | A2 residual - `texttype_impl = nullptr`, sem `fn_destroy` | FEITO |
| 6 | M2 - `TEXTTYPE_ENTRY3` | FEITO |
| 7 | Suíte de testes auto-verificável (`test_ltrim_zero.sql`, 55 asserts) | FEITO |
| 8 | M5 - apagar `src/burp/restore - Copia*.epp` | PENDENTE (fora do escopo da collation) |
| 9 | O0 - reconstruir índices antes/depois de trocar a lib em produção | PENDENTE (operacional) |
| 10 | O1 - re-rodar cmake / `CONFIGURE_DEPENDS` em build tree existente | PENDENTE (operacional) |

## Como rodar a suíte

```bat
msbuild builds\win32\msvc15\intl.vcxproj /p:Configuration=Release /p:Platform=x64
del test_ltrim.fdb
set FIREBIRD=D:\GitHub\firebird\temp\x64\Release\firebird
temp\x64\Release\firebird\isql.exe -input test_ltrim_zero.sql
```

A suíte passa quando o rodapé imprime `*** SUITE PASSED ***` e `FAILED = 0`. Checks marcados `I` (info) documentam limites conhecidos e nunca reprovam.

## DDL de uso

```sql
CREATE COLLATION WIN1252_LTRIM_ZERO FOR WIN1252
    FROM EXTERNAL ('WIN1252_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE;
```

`CASE INSENSITIVE` não é opcional na prática: sem ele o `SIMILAR TO` fica case-sensitive enquanto `=`, `LIKE` e `CONTAINING` não ficam (ver A4). `PAD SPACE` é o que faz coluna `CHAR` funcionar (ver C1).

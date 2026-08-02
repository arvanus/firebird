# LTRIM_ZERO: ordenação numérica por padrão

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

## 5. STARTING WITH

Com a chave prefixada pelo tamanho, a chave de um prefixo deixa de ser prefixo da chave
do valor inteiro, então busca por prefixo com índice passa a não encontrar as linhas.
O otimizador não pergunta: em `Retrieval.cpp` (por volta de 885) ele marca
`usePartialKey` sempre que o segmento é `segmentScanStarting`, e não existe flag para a
collation declarar que não sabe fazer chave parcial.

Decisão: `string_to_key` devolve `INTL_BAD_KEY_LENGTH` quando `key_type` é
`INTL_KEY_PARTIAL`. O engine aborta a consulta com erro em vez de devolver linhas a
menos em silêncio. A mensagem fala em tamanho de chave, o que é fora de contexto, mas
é preferível a resultado errado silencioso.

`INTL_KEY_MULTI_STARTING` não se aplica: a collation não declara
`TEXTTYPE_MULTI_STARTING_KEY`. `INTL_KEY_UNIQUE` e `INTL_KEY_SORT` recebem a mesma
chave prefixada. Como a collation também não declara `TEXTTYPE_SEPARATE_UNIQUE`,
chave parcial só é pedida em `STARTING WITH`: igualdade e `BETWEEN` não passam por
esse caminho.

`LIKE 'x%'` é convertido em `blr_starting` pelo otimizador (`Optimizer.cpp:1358`)
quando há índice, então cai na mesma regra. `LIKE` resolvido por varredura continua
funcionando normalmente, porque usa comparação, não chave.

## 6. Compatibilidade

**Todo índice existente sobre coluna com essa collation guarda chave no formato
antigo.** Depois de trocar o módulo, esses índices apontam para a ordem velha e passam
a devolver resultado errado até serem reconstruídos. Isso vale para:

- `SCHERER_001.FDB`: 421 segmentos de índice sobre as 368 colunas do domínio `TDR_CNPJ`.
- Qualquer outro banco do servidor que use `ISO8859_1_LTRIM_ZERO`, `WIN1252_LTRIM_ZERO`,
  `UTF8_LTRIM_ZERO`, `NONE_LTRIM_ZERO` ou `DOS850_LTRIM_ZERO`.

A ordem de troca é: parar o serviço, trocar o dll, subir, reconstruir os índices, e só
então liberar o uso. Reconstruir com `ALTER INDEX ... INACTIVE` seguido de
`ALTER INDEX ... ACTIVE`, ou com um restore completo, que reconstrói tudo.

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

Unitários no harness que já existe para a collation (`src/jrd/tests/LtrimZeroCollationTest.cpp`)
mais checagem em SQL:

1. `9` antes de `0001A34` em `ORDER BY`.
2. Dentro do mesmo tamanho: `9` antes de `A`; `a` e `A` na mesma posição.
3. Normalizado vazio primeiro.
4. Igualdade preservada: `'000123' = '123'`, `'0000009' = 9`.
5. `BETWEEN '12345678000000' AND '12345678999999'` não traz o valor de 11 dígitos.
6. Índice contra varredura (`col` versus `col || ''`) com o mesmo resultado, em
   igualdade, `BETWEEN` e `ORDER BY`, sobre volume que force uso de índice.
7. `STARTING WITH` sobre coluna indexada aborta com erro, e não devolve resultado
   parcial.
8. Chave e `compare()` concordando: para cada par do conjunto de teste, o sinal de
   `compare()` bate com a comparação byte a byte das chaves.

O teste 8 é o que segura a invariante que sustenta tudo: índice e comparação têm que
contar a mesma história.

## 9. Riscos

- Alguma consulta da aplicação usar `STARTING WITH` ou `LIKE 'x%'` sobre coluna do
  domínio. No código armazenado do banco do cliente não há nenhum `STARTING WITH`
  (0 procedures, 0 triggers, 0 views) e só 4 procedures usam `LIKE`, mas o SQL que a
  aplicação monta não dá para inspecionar daqui. Depois da troca, esse caso passa a dar
  erro, o que é visível e corrigível, e não silencioso.
- Reconstrução de índice em base grande é demorada e precisa de janela.
- Ordem de relatório muda para quem já dependia da ordem alfabética atual.

# Rollout da ordem numérica na LTRIM_ZERO

A ordem da collation mudou: a chave passou a carregar o tamanho normalizado
antes dos bytes, então `9` vem antes de `10` e não depois. O detalhe do
formato e a motivação estão em [README.ltrim_zero.md](README.ltrim_zero.md);
a receita de compilação do módulo está em
[README.fbltrimzero_build.md](README.fbltrimzero_build.md).

Este documento é a janela de troca: o que quebra, por que o caminho é backup
e restore, a sequência exata e como conferir depois.

Números citados aqui foram medidos em 2026-08-03 contra
`D:\u\banco\scherer\SCHERER_001.FDB` com o Firebird 5.0 instalado.

## 0. Inventário antes de tudo

Rodar [ltrim_zero_rollout_inventory.sql](ltrim_zero_rollout_inventory.sql)
em **cada banco do servidor**, não só no que se sabe afetado. O módulo é
compartilhado por toda a instalação.

```
"C:\Program Files\Firebird\Firebird_5_0\isql.exe" -u SYSDBA -p masterkey ^
  <caminho do banco> ^
  -i doc\ltrim_zero_rollout_inventory.sql ^
  -o inventario_<banco>.txt
```

O inventário resolve tudo por `RDB$COLLATIONS.RDB$BASE_COLLATION_NAME`, que
guarda o nome do `FROM EXTERNAL` (`src/dsql/DdlNodes.epp:3977`), nunca pelo
nome local: `CREATE COLLATION ... FROM EXTERNAL` deixa o DBA batizar a
collation como quiser, e a própria suíte deste repositório usa o nome `LTZ`.
Procurar por `ISO8859_1_LTRIM_ZERO` em `RDB$COLLATION_NAME` perderia esse
banco em silêncio.

Resultado no `SCHERER_001`:

| item | valor |
|---|---|
| collations do módulo | 1 (`ISO8859_1_LTRIM_ZERO`, id 126, charset 21) |
| colunas atingidas | 368, todas do domínio `TDR_CNPJ` |
| segmentos de índice | 421 (216 PRIMARY KEY, 2 UNIQUE, 203 sem constraint) |
| segmentos em índice único | 220, sendo 2 índices únicos sem constraint |
| segmentos descendentes | 2 |
| segmentos em índice inativo | 0 |
| índices de expressão no banco | 38 |
| folga de chave | `KEY_LIMIT` 4096 (página 16384) contra pico estimado de 778 bytes |

Dos 16 bancos em `D:\u\banco\scherer`, só o `SCHERER_001` tem a collation.
Todos os outros devolveram zero na query 1.

A query 5 é limite superior grosseiro, não a conta do motor: cada segmento
afetado cresce 2 bytes, índice descendente soma 1, e índice composto
amplifica em cerca de 25% dos bytes de stuff (`src/jrd/btr.cpp:1727`). O
limite é `page_size / 4` (`src/jrd/Database.h:652-655`). Neste banco a folga
é de mais de cinco vezes, então nenhum índice entra no regime de truncamento.

A query 4 existe porque índice de expressão não guarda linha em
`RDB$INDEX_SEGMENTS` (conferido: 38 índices de expressão, 0 segmentos), então
a query 3 não enxerga nenhum deles. O motor monta a chave desses índices a
partir do resultado da expressão, pelo caminho de segmento único
(`src/jrd/btr.cpp:2059`), então o que decide é a collation do resultado. A
fonte de cada um tem que ser lida à mão e cruzada com a lista da query 2.

## 1. O que quebra

Todo índice sobre coluna com a collation guarda chave no formato antigo.
Depois de trocar o dll, esses índices descrevem uma ordem que o driver não
fala mais, e o motor não tem como perceber isso: chave de índice é bytes
opacos, sem versão gravada.

Medido no `SCHERER_001` em 2026-08-03, com o dll novo instalado e os índices
ainda como estavam:

| consulta | via índice | via varredura (`CNPJ \|\| ''`) |
|---|---|---|
| `CNPJ = '06056181000154'` | 0 linhas | 1 linha |
| `CNPJ BETWEEN '10000000000000' AND '99999999999999'` | 0 linhas | 139433 linhas |
| `SELECT CNPJ FROM ENTIDADE ORDER BY CNPJ` | 0 de 544149 | 544149 |

A última linha é sem `PLAN` nenhum: o otimizador escolhe sozinho navegar pelo
índice e a consulta devolve zero linha de 544149. Forçar
`PLAN (ENTIDADE ORDER PK_ENTIDADE)` dá o mesmo zero;
`PLAN (ENTIDADE NATURAL)` devolve as 544149.

Nenhuma dessas consultas deu erro. O banco responde depressa e responde
errado. É esse o estado em que a instalação fica entre a troca do dll e o
fim da reconstrução dos índices.

## 2. Por que backup e restore, e não `ALTER INDEX`

Reconstruir índice a índice exigiria desativar e reativar cada um, e
`ALTER INDEX ... INACTIVE` é recusado para índice de constraint:

```
Statement failed, SQLSTATE = 27000
unsuccessful metadata update
-ALTER INDEX RDB$PRIMARY1 failed
-action cancelled by trigger (3) to preserve data integrity
-Cannot deactivate index used by a PRIMARY/UNIQUE constraint
```

O gatilho de sistema `RDB$TRIGGER_20` é registrado três vezes, como
`integ_index_mod`, `integ_index_deactivate` e `integ_deactivate_primary`
(`src/jrd/ini.epp:188-190`); o texto das mensagens está no bloco comentado de
referência do `trigger20`, em `src/jrd/trig.h:1376-1378`. Índice comum, sem
constraint, desativa normalmente.

Dos 421 segmentos, 218 estão sob constraint (216 de PRIMARY KEY e 2 de
UNIQUE). Um rebuild seletivo deixaria justamente esses no formato antigo com
o dll novo, que é o estado de resultado errado da seção 1, não o estado
lento. Restam 203 segmentos sem constraint, que até dariam para tratar por
`ALTER INDEX`, mas fazer metade do trabalho não ajuda.

O restore reconstrói todo índice chamando o driver de novo, que já é o novo.
É uma operação só, sem exceção por tipo de índice.

## 3. Nenhuma escrita entre trocar o dll e terminar o rebuild

A janela é de **indisponibilidade total**, não de somente leitura.

**Unicidade.** `insert_key` entrega a chave nova ao `BTR_insert`, que decide
quem é candidato a duplicata comparando contra as chaves já gravadas na
página, ainda no formato antigo; `check_duplicates` só enxerga o que o BTR
marcou (`src/jrd/idx.cpp:2112-2119`). Com 218 segmentos de constraint mais 2
índices únicos sem constraint, um `INSERT` nessa janela pode gravar duplicata
real numa PRIMARY KEY sem erro nenhum.

**Integridade referencial.** `check_foreign_key` monta a chave nova e procura
no índice do parceiro pelo `BTR_evaluate`, também no formato antigo
(`src/jrd/idx.cpp:1942-1971`). No `SCHERER_001` isso não se aplica: o domínio
não participa de nenhuma FK. Vale para qualquer outro banco que o inventário
apontar, e é por isso que a seção 9 não é formalidade.

Nos dois casos a violação entra calada e só aparece depois, quando o índice
reconstruído passar a enxergar o conflito.

## 4. Sequência da janela

1. **Parar a aplicação** e conferir que não sobrou conexão:

   ```sql
   SELECT COUNT(*) FROM MON$ATTACHMENTS;
   ```

   Lembrando que `gbak -PAR` abre uma conexão por worker, então esse
   contador infla durante o backup.

2. **Backup do estado atual.** Ele também é o rollback.

   ```
   "C:\Program Files\Firebird\Firebird_5_0\gbak.exe" -b -PAR 5 -v ^
     -y D:\u\banco\scherer\bkp_pre_numeric.log ^
     D:\u\banco\scherer\SCHERER_001.FDB ^
     D:\u\banco\scherer\scherer_001_pre_numeric.fbk
   ```

   O gbak instalado serve. O gbak compilado deste repositório só é necessário
   para `-FIX_DOMAINS`, que já foi usado na conversão do domínio e não entra
   aqui.

3. **Trocar o dll**, com o serviço parado:

   ```powershell
   Stop-Service FirebirdServerDefaultInstance
   $intl = 'C:\Program Files\Firebird\Firebird_5_0\intl'
   Copy-Item "$intl\fbltrimzero.dll" "$intl\fbltrimzero.dll.bak" -Force
   Copy-Item 'D:\u\banco\scherer\fbltrimzero_numeric-order.dll' "$intl\fbltrimzero.dll" -Force
   Start-Service FirebirdServerDefaultInstance
   ```

   O binário validado é o da Task 5 do plano, gerado por
   `builds\win32\make_ltrimzero.bat` e conferido contra a suíte SQL rodando
   sobre engine estoque. O `fbltrimzero.conf` não muda e não precisa ser
   recopiado.

4. **Restore por cima de um caminho novo:**

   ```
   "C:\Program Files\Firebird\Firebird_5_0\gbak.exe" -c -PAR 5 -v ^
     -y D:\u\banco\scherer\restore_numeric.log ^
     D:\u\banco\scherer\scherer_001_pre_numeric.fbk ^
     D:\u\banco\scherer\SCHERER_001.NEW.FDB
   ```

5. **Renomear e subir a aplicação:**

   ```
   SCHERER_001.FDB      ->  SCHERER_001.FDB.pre_numeric
   SCHERER_001.NEW.FDB  ->  SCHERER_001.FDB
   ```

## 5. Validação depois do restore

Linha de base reconferida em 2026-08-03 contra o `SCHERER_001` atual. Os
mesmos números têm que sair do banco restaurado:

| conferência | valor |
|---|---|
| `RDB$RELATIONS` de usuário | 1242 (1196 tabelas + 46 views) |
| `RDB$PROCEDURES` | 885 |
| `RDB$TRIGGERS` de usuário | 1812 |
| `RDB$INDICES` de usuário | 2231 |
| índices inativos | 7 (já eram assim antes da conversão do domínio) |
| triggers com `RDB$VALID_BLR = 0` | 2 (idem) |
| colunas no domínio `TDR_CNPJ` | 368 |
| índices de expressão | 38 |
| `ENTIDADE`: linhas / CNPJ distintos | 544149 / 544149 |
| `ENTIDADE`: `SUM(CAST(CNPJ AS BIGINT))` | 6151677092957447095 |
| `ENTIDADE`: `MAX(CHAR_LENGTH(CNPJ))` | 15 |

O `SUM` precisa do `CAST` explícito: a coluna é `VARCHAR(20)` desde a
conversão do domínio, e `SUM` sobre texto é recusado em dialeto 3
(`Argument for SUM in dialect 3 must be numeric`). O runbook da conversão
registra esse total sem o cast porque na época a coluna ainda era numérica.

**Índice contra varredura.** Cada par tem que devolver o mesmo número. É o
que prova que os índices foram reconstruídos:

```sql
SELECT COUNT(*) FROM ENTIDADE WHERE CNPJ = '06056181000154';        -- usa índice
SELECT COUNT(*) FROM ENTIDADE WHERE CNPJ || '' = '06056181000154';  -- força varredura

SELECT COUNT(*) FROM ENTIDADE
 WHERE CNPJ BETWEEN '10000000000000' AND '99999999999999';
SELECT COUNT(*) FROM ENTIDADE
 WHERE CNPJ || '' BETWEEN '10000000000000' AND '99999999999999';    -- espera 139433
```

**Ordem nova.** As duas listas têm que sair idênticas, e em ordem numérica
(`1, 2, 3, 5, 6, 8, 9, 10, 12, ...`, não `1, 10, 12, ..., 2, 3`):

```sql
SELECT CNPJ FROM ENTIDADE PLAN (ENTIDADE ORDER PK_ENTIDADE) ORDER BY CNPJ ROWS 12;
SELECT CNPJ FROM ENTIDADE PLAN (ENTIDADE NATURAL)           ORDER BY CNPJ ROWS 12;
```

Antes do rebuild a primeira devolve zero linha, como registrado na seção 1.

**Faixa não alcança outro tamanho.** Com a ordem por tamanho, uma faixa entre
dois valores de 14 dígitos não pode trazer valor de tamanho normalizado
diferente, então isto tem que dar 0:

```sql
SELECT COUNT(*) FROM ENTIDADE
 WHERE CNPJ BETWEEN '10000000000000' AND '99999999999999'
   AND CHAR_LENGTH(TRIM(LEADING '0' FROM CNPJ)) <> 14;
```

Esse teste só tem valor depois que o par de `BETWEEN` acima estiver batendo:
enquanto o índice estiver velho ele dá 0 pelo motivo errado, porque a faixa
não devolve linha nenhuma.

**Views.** As três views que expõem colunas do domínio (`VCONTAS_A_PAGAR`,
`VCONTAS_A_RECEBER`, `V$PRODUTO_ESTOQUE_ANALISE_01`) já foram usadas como
conferência na conversão do domínio e servem de novo: têm que responder sem
erro, o que mostra que o BLR guardado continua válido.

## 6. `gfix -v` durante a janela

Entre a troca do dll e o fim do restore, `gfix -v` reporta corrupção de
índice. É esperado: o validador lê as chaves com o driver novo. Não indica
problema novo e não é motivo para abortar.

## 7. Rollback

Enquanto ninguém tiver gravado no banco novo, é reversível sem perda:

```
SCHERER_001.FDB              ->  SCHERER_001.FDB.pos_numeric
SCHERER_001.FDB.pre_numeric  ->  SCHERER_001.FDB
```

e restaurar o dll:

```powershell
Stop-Service FirebirdServerDefaultInstance
Copy-Item "$intl\fbltrimzero.dll.bak" "$intl\fbltrimzero.dll" -Force
Start-Service FirebirdServerDefaultInstance
```

Os dois passos andam juntos: banco antigo com dll novo é o estado de
resultado errado da seção 1.

Depois que a aplicação começar a gravar no banco novo, voltar atrás custa
essas gravações. O `.fbk` do passo 2 reconstrói o estado anterior do zero.

## 8. Efeitos permanentes a comunicar ao cliente

- **`STARTING WITH` e `LIKE 'x%'` sobre coluna indexada passam a varrer o
  índice inteiro.** O resultado continua correto e nenhuma linha se perde
  (asserts 8.6, 8.6b, 8.6c e 8.7 da suíte), mas o plano piora, e o otimizador
  ainda estima a faixa como seletiva por `REDUCE_SELECTIVITY_FACTOR_STARTING`
  (`src/jrd/optimizer/Retrieval.cpp:990`), então ele pode escolher esse índice
  achando que filtra. O detalhe está em
  [README.ltrim_zero.md](README.ltrim_zero.md).
- **`BETWEEN` muda de significado.** Para CNPJ de largura fixa a mudança é o
  que se quer, mas onde a coluna mistura valores de tamanhos diferentes vale
  revisar cada faixa: agora a faixa é por tamanho primeiro.
- **Relatório que dependia da ordem alfabética muda.** `ORDER BY` sobre essas
  colunas passa a sair em ordem numérica.
- Quem quiser a ordem antiga em algum relatório específico pode pedir
  `ORDER BY col COLLATE ISO8859_1`, que não passa pelo driver.

## 9. Outros bancos do servidor

O dll é compartilhado por toda a instalação: trocar o módulo muda o
comportamento de **todo** banco daquele servidor que use a collation, não só
o alvo. Rodar o inventário em cada um e incluir na mesma janela todo banco
cuja query 1 não vier vazia.

No servidor de desenvolvimento, dos 16 bancos em `D:\u\banco\scherer` só o
`SCHERER_001` usa a collation. O servidor do cliente tem que ser inventariado
de novo, não dá para reaproveitar essa conclusão.

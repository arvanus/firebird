# gbak: remapeamento de domínio no restore (-FIX_DOMAINS)

> **Rename note.** This is a dated record, written while the companion collation was
> called `LTRIM_ZERO` and its standalone module `fbltrimzero`. Names below are the
> ones in use at the time and were deliberately left untouched. The collation is now
> `ID_ZPAD_CI` (`WIN1252_ID_ZPAD_CI`, `ISO8859_1_ID_ZPAD_CI`) and the module `lrsintl`;
> see `doc/README.id_zpad_ci.md` on the collation branch. Nothing about `-FIX_DOMAINS`
> itself changed.

Data: 2026-08-01
Branch: `srs/gbak-domain-remap` (a partir de `v5.0-release`)
Alvo: build interno próprio, desenhado para ser submetível upstream (ver seção 12.4)

## 1. Contexto

Existe um hack local em `gbak_hacked` (commit `acd92753b7`, também em `fork/gbak_hacked`) que altera
o domínio `TDR_CNPJ` durante o restore, convertendo de `NUMERIC(18,0)` para `VARCHAR(17)` com charset
e collation fixos. O hack é incondicional, tem o nome do domínio compilado no binário, reaproveita a
mensagem 121 do catálogo (`"restoring domain @1"`) para imprimir 8 linhas de debug por domínio, e
codifica o `RDB$COLLATION_ID` como número literal.

O objetivo aqui é transformar essa necessidade real em um recurso genérico, seguro e submetível
upstream.

### 1.1 O que já está provado e o que não está

**Provado:** a conversão de dados funciona. Com o domínio redefinido antes da criação das relations,
o engine converte os valores `BIGINT` do stream para texto na inserção, e os dados chegam legíveis
na coluna. Não é necessário mexer em `RDB$RELATION_FIELDS` nem em `RDB$FORMATS`.

**Não provado:** o binding da collation. Na execução registrada em `debugGbakOutput.txt`, o domínio
`TDR_CNPJ` saiu com `COLLATION_ID=16`, que em ISO8859_1 é uma collation built-in, e não a
`ISO8859_1_LTRIM_ZERO_AI` pretendida (que naquele backup tem id 126). Ou seja: a semântica de
comparação LTRIM_ZERO nunca foi exercitada de ponta a ponta em um domínio convertido. É isso que
justifica a validação semântica da seção 8.

### 1.2 Precedente upstream

`FIX_FSS_DATA` e `FIX_FSS_METADATA` (`src/burp/burpswi.h:136,139`) são switches de restore que
corrigem metadados durante a restauração, cada um com constante de serviço própria
(`isc_spb_res_fix_fss_data = 13`, `isc_spb_res_fix_fss_metadata = 14`, em
`src/include/firebird/impl/consts_pub.h:555-556`) e mensagem própria no catálogo. O recurso proposto
segue exatamente esse molde.

## 2. Objetivo

Permitir que o operador declare, na linha de comando do restore, um conjunto de regras que redefinem
domínios enquanto o backup é restaurado, referenciando charsets e collations **por nome**, com falha
explícita quando qualquer regra não puder ser satisfeita.

## 3. Não-objetivos

- Conversão no nível de registro (`RDB$RELATION_FIELDS`, `RDB$FORMATS`). Desnecessária: o engine já
  converte na inserção.
- Alterar colunas individualmente. O escopo é o domínio; colunas herdam.
- Tipos além de `VARCHAR(n)` e `CHAR(n)` nesta primeira versão. Qualquer outro tipo alvo é recusado
  na validação sintática, com mensagem clara.
- Tratamento especial para valores que não caibam no tipo alvo. O erro de conversão do engine
  propaga e o restore falha, que é o comportamento padrão do gbak diante de erro de dado.

## 4. Ordem do stream e o ponto de aplicação

A emissão de metadados no backup é fixa em código, não incidental
(`src/burp/backup.epp:345,355,359,366`):

```
write_global_fields()    -> rec_global_field   (domínios)
write_character_sets()   -> rec_charset
write_collations()       -> rec_collation
write_relations()        -> rec_relation       (tabelas e colunas)
```

No restore, `rec_relation_data` é quem emite `BURP_verbose(68)` ("committing metadata") e faz o
`COMMIT` (`src/burp/restore.epp:10731-10743`), já depois das relations.

Consequência: quando um domínio é lido, as collations do backup **ainda não foram restauradas**, e
por isso o hack original precisou de um id literal. Mas quando a primeira relation é lida, as
collations **já estão gravadas** em `RDB$COLLATIONS` pela transação corrente, e nenhum formato de
relation foi calculado ainda.

Daí o desenho em duas etapas:

| Etapa | Onde | O que faz |
|---|---|---|
| Aplicação | `get_global_field()`, nos três ramos ODS | casa o nome do domínio com as regras; aplica tipo, tamanho, escala, subtipo, precisão e `CHARACTER_LENGTH`; grava com collation provisória; registra pendência |
| Resolução | topo de `case rec_relation:` (`restore.epp:10670`) | verifica que toda regra gerou exatamente uma pendência; resolve charset e collation **por nome** consultando `RDB$CHARACTER_SETS` e `RDB$COLLATIONS` no próprio banco destino; faz `MODIFY` em `RDB$FIELDS` |

A resolução é idempotente e é chamada em três pontos, para cobrir backups degenerados: topo de
`case rec_relation:`, início de `case rec_relation_data:`, e fim do restore antes do commit final.
O terceiro garante que um backup sem nenhuma relation ainda assim aborte quando uma regra não for
satisfeita, inclusive sob `-m`.

### 4.1 Prova de que o ponto de resolução é seguro

O formato da relation não é calculado no `STORE` da relation, e sim no commit da transação:
`make_version` é o handler registrado para `dfw_update_format` (`src/jrd/dfw.epp:1249`), executado
como deferred work. É nesse momento que ele lê `RDB$RELATION_FIELDS CROSS RDB$FIELDS` e extrai
`FLD.RDB$COLLATION_ID`. Como o `MODIFY` acontece antes da criação de qualquer relation, o formato é
montado a partir do `RDB$FIELDS` já corrigido.

O primeiro commit de metadados acontece **dentro de `get_relation()`**, na primeira relation e antes
de armazená-la (`restore.epp:7769-7783`, guardado por `if (!tdgbl->relations)`), e não em
`rec_relation_data` como se poderia supor pela mensagem 68. O ponto de resolução roda no topo de
`case rec_relation:`, portanto antes dessa chamada, e cai na mesma transação que gravou os domínios
e as collations. A margem é essa, e não é grande: uma implementação que coloque a resolução dentro
de `get_relation()` fica depois do commit e muda a semântica transacional sem avisar.

Existe ainda um commit por tabela em `restore.epp:7992-8009`, também dentro de `get_relation()`,
ativo quando `gbl_sw_incremental` está ligado. Igualmente posterior ao ponto de resolução.

### 4.2 Limite conhecido: collation na coluna vence a do domínio

Ainda em `make_version`, `RFR.RDB$COLLATION_ID` (de `RDB$RELATION_FIELDS`) sobrescreve
`FLD.RDB$COLLATION_ID` quando não é nulo. Uma coluna que traga collation própria no backup **não**
segue o remap do domínio. No caso que motivou o recurso isso não ocorre, porque o domínio de origem
é numérico e as colunas não têm collation. Ainda assim, o relatório final deve apontar colunas que
usam um domínio remapeado e carregam collation própria, já que para elas o resultado difere do
esperado.

Não há necessidade de manter mapa de nomes em memória nem de reler o arquivo: a consulta ao banco
destino no momento certo resolve os dois casos (collations de usuário vindas do backup e charsets
built-in que já existem desde a criação do banco).

## 5. Interface

### 5.1 Switch

`-FIX_DOMAINS <regras>`, `boRestore`, abreviação mínima de 5 caracteres (`-FIX_D`), o que já o
distingue dos `FIX_FSS_*` (que exigem 9). Constante de serviço nova `isc_spb_res_fix_domains` em
`consts_pub.h`, seguindo a numeração livre subsequente.

O argumento aceita duas formas:

```
-FIX_DOMAINS @d:\u\banco\remap.conf
-FIX_DOMAINS "TDR_CNPJ = VARCHAR(20) CHARACTER SET ISO8859_1 COLLATE ISO8859_1_LTRIM_ZERO_AI"
```

O prefixo `@` indica arquivo. Sem `@`, o próprio argumento é o conjunto de regras, separadas por
`;`. A forma inline evita que um arquivo precise existir no servidor quando o restore for disparado
por Services API.

**Services API fica fora do escopo da primeira versão.** Um tag SPB novo com argumento string exige
também um `case` em `ClumpletReader.cpp` e outro em `svc.cpp`, sem os quais o SPB é rejeitado com
`invalid_structure`. Portanto a entrada na tabela de switches usa `0` no campo `in_spb_sw`, como
`USER` e `PASSWORD`, e o switch funciona apenas por linha de comando. A constante
`isc_spb_res_fix_domains` fica definida para quando o caminho de serviço for ligado.

### 5.2 Formato das regras

```
# comentário até o fim da linha
<NOME_DO_DOMINIO> = VARCHAR(<n>) [CHARACTER SET <nome>] [COLLATE <nome>]
<NOME_DO_DOMINIO> = CHAR(<n>)    [CHARACTER SET <nome>] [COLLATE <nome>]
```

Sem números de charset ou collation em lugar nenhum. `COLLATE` sem `CHARACTER SET` usa o charset
default do banco. `CHARACTER LENGTH` é derivado de `n` e do número de bytes por caractere do charset.

### 5.2.1 Largura mínima: mesma regra do DDL

`n` precisa ser maior ou igual ao que `AlterDomainNode::checkUpdate` exigiria para o tipo de origem,
ou seja `DSC_string_length(&origFld.dyn_dsc)` (`src/dsql/DdlNodes.epp:4441-4442`). Para `BIGINT` isso
dá 20. Regra que peça menos é recusada na aplicação, com a mesma mensagem do DDL
(`isc_dyn_char_fld_too_small`).

Consequência prática para o caso que motiva o recurso: a regra declara `VARCHAR(20)`, não
`VARCHAR(17)`. Se a intenção for limitar o conteúdo a 17 caracteres, isso é feito com uma constraint
depois do restore, e não estreitando o tipo:

```sql
ALTER DOMAIN TDR_CNPJ ADD CONSTRAINT CHECK (CHAR_LENGTH(VALUE) <= 17);
```

Verificado: `ALTER DOMAIN ... TYPE VARCHAR(17)` sobre `BIGINT` falha com "New size specified for
column TDR_CNPJ must be at least 20 characters"; com `VARCHAR(20)` mais o `CHECK` acima, um valor de
17 caracteres é aceito e um de 18 é barrado com `validation error`.

Se o domínio de origem já traz `RDB$VALIDATION_BLR` numérico (seção 12.3), remover a constraint
antiga antes de adicionar a nova.

Verificado também que a largura não impede integridade referencial: uma FK entre coluna
`VARCHAR(17)` e coluna `VARCHAR(20)` de mesmo charset e collation é criada e opera normalmente. O
que a FK compara é o `idx_itype`, não o comprimento. Portanto manter `VARCHAR(20)` não quebra
convivência com um domínio modelo de 17.

### 5.2.2 Override UNCHECKED

O gbak nunca chama `checkUpdate`, porque grava direto em `RDB$FIELDS`. A regra de largura mínima é,
portanto, uma salvaguarda deste recurso, e não uma restrição herdada do caminho DDL. A palavra-chave
`UNCHECKED` ao fim de uma regra a desliga para aquela regra:

```
TDR_CNPJ = VARCHAR(17) CHARACTER SET ISO8859_1 COLLATE ISO8859_1_LTRIM_ZERO_AI UNCHECKED
```

Quando usada, o gbak emite aviso identificando o domínio, a largura pedida e a mínima que o DDL
exigiria, e prossegue. Duas consequências que precisam estar claras para quem usa:

- A objeção da seção 12.2 volta a valer para as regras que levam `UNCHECKED`: o domínio resultante é
  um estado que `ALTER DOMAIN` recusaria.
- O truncamento passa a ser descoberto tarde. Um valor que não caiba falha durante o carregamento
  dos dados, com erro de conversão do engine, e não na validação das regras. Em backup grande, isso
  significa perder o trabalho de várias horas.

A recomendação continua sendo declarar a largura mínima e restringir o conteúdo por constraint. O
`UNCHECKED` existe para o caso em que o operador conhece os dados e quer o tipo estreito assim mesmo.

### 5.3 Preview

Não há switch de dry-run. O `-m` (metadata only) que já existe cumpre o papel:

```
gbak -m -c d:\u\banco\dry.fdb backup.fbk -FIX_DOMAINS @remap.conf -v
```

Restaura apenas metadados, aplica o remap de verdade, imprime o de-para com os ids já resolvidos e
para antes dos dados. Valida exatamente as mesmas condições do restore completo: domínio presente,
charset e collation resolvidos, contagem do `MODIFY` correta. O banco resultante é descartável.

## 6. Módulo

Código novo isolado em `src/burp/domain_remap.h` / `domain_remap.cpp`, sem dependência de GPRE:

- parse do argumento (`@arquivo` ou inline) e das regras
- `struct RemapRule`: nome do domínio, tipo alvo, tamanho, nome do charset, nome da collation
- lista de pendências preenchida durante a aplicação
- comparação de nome de domínio tratando `CHAR(63)` com padding de espaço de forma explícita

Em `restore.epp` ficam apenas os ganchos e a função de resolução, que precisa de GPRE por causa das
consultas às system tables.

O arquivo novo precisa ser registrado nos sistemas de build junto com o código, e não depois:
`builds/win32/msvc15/burp.vcxproj` e o `.filters` correspondente, mais as listas de fontes do CMake
e do POSIX. Na branch da collation isso virou um commit de correção separado
(`d87ace7ffa`, "Add lc_ltrim_zero.cpp to MSVC intl project"); aqui entra de primeira.

Os tipos alvo devem usar as constantes `blr_varying` e `blr_text`, e não o literal `37` que o hack
original gravava.

A aplicação nos três ramos ODS (`restore.epp:5412` para `>=DDL12`, `5785` para `>=DDL10`, `6135`
para o restante) chama um único helper. Os três gravam `RDB$CHARACTER_SET_ID` e `RDB$COLLATION_ID`,
então não há ramo sem suporte. Os ramos legados não são código morto: `runtimeODS` é detectado do
banco destino (`src/burp/OdsDetection.epp:118-130`), logo um gbak 5 restaurando contra um servidor
antigo cai neles.

## 7. Condições de erro

Todas abortam antes do commit de metadados, de modo que nenhum banco chega a existir em estado
inconsistente:

| Condição | Momento |
|---|---|
| Sintaxe inválida nas regras, ou tipo alvo não suportado | antes de criar o banco |
| Domínio já está no formato alvo | aplicação |
| Regra que não gerou exatamente uma pendência | resolução |
| Charset ou collation não resolvido por nome | resolução |
| `MODIFY` afetou número de linhas diferente do esperado | resolução |

"Domínio já está no formato alvo" só é detectável na aplicação: depois dela o tipo original do
backup já foi sobrescrito e não há mais com o que comparar.

A verificação decisiva é a de pendências, não a contagem do `MODIFY`. O `MODIFY` opera sobre linhas
que o próprio gbak acabou de gravar, então casa por construção e não diz nada sobre acerto de nome.
O risco real de não casar vive na aplicação, em `get_global_field()`: uma regra cujo nome de domínio
nunca casou produz zero pendências, e sem essa checagem o restore seguiria em silêncio com o domínio
intacto ou com collation 0. Foi essa classe de falha silenciosa que produziu a `COLLATION_ID=16` da
seção 1.1. A contagem do `MODIFY` permanece como asserção de sanidade.

## 8. Validação

**Estrutural**, feita pelo próprio gbak e reportada ao final: join de `RDB$FIELDS` com
`RDB$COLLATIONS` confirmando que o **nome** da collation aterrissou no domínio, a contagem de
domínios alterados contra o número de regras, e a lista de colunas que usam um domínio remapeado mas
carregam `RDB$COLLATION_ID` próprio (seção 4.2).

**Semântica**, feita pelo operador após o restore, porque metadado correto com collation errada é
exatamente o caso que já ocorreu:

```sql
SELECT c.RDB$COLLATION_NAME
  FROM RDB$COLLATIONS c
  JOIN RDB$FIELDS f ON f.RDB$COLLATION_ID = c.RDB$COLLATION_ID
                   AND f.RDB$CHARACTER_SET_ID = c.RDB$CHARACTER_SET_ID
 WHERE TRIM(f.RDB$FIELD_NAME) = 'TDR_CNPJ';
-- esperado: ISO8859_1_LTRIM_ZERO_AI

SELECT COUNT(*) FROM <tabela> WHERE <coluna_cnpj> = '000123';
-- tem que encontrar a linha gravada como '123'
```

Pré-condição a documentar: a collation nomeada precisa existir no servidor que restaura. Uma
collation derivada de plugin (caso da LTRIM_ZERO) exige o `fbintl` correspondente instalado, e sua
ausência falha no momento do dado, não no do metadado.

Nota sobre os dois nomes que aparecem neste documento. O plugin registra
`ISO8859_1_LTRIM_ZERO`, conforme `builds/install/misc/fbintl.conf` na branch
`feature/ltrim-zero-collation-v5`. Já `ISO8859_1_LTRIM_ZERO_AI`, visto em
`debugGbakOutput.txt:38220`, é uma collation de usuário existente no backup do cliente, derivada
daquela com atributos de accent insensitive. A regra deve nomear a que de fato existirá no banco
restaurado: a do backup, quando o backup a traz.

## 9. Mensagens

Entradas novas em `src/include/firebird/impl/msg/gbak.h`, nos próximos números livres, para:
descrição do switch na ajuda, linha de de-para por domínio remapeado, resumo final, e cada condição
de erro da seção 7. Nenhuma mensagem existente é reaproveitada.

## 10. Código removido em relação ao hack

- O bloco de debug de 8 `BURP_verbose(121, ...)` por domínio. Reaproveitava mensagem alheia e gerou
  cerca de 30 mil linhas de saída em um único restore. O diagnóstico equivalente passa a sair do
  relatório de remap e do preview com `-m`.
- O marcador `//Copiar aqui!`. O remap passa a existir nos três ramos via helper compartilhado.
- O `strncmp` com tamanho literal 8 e a comparação manual de `' '`/`'\0'`, substituídos pela
  comparação de nome do módulo novo.

## 11. Branches

```
v5.0-release
├── feature/ltrim-zero-collation-v5   (collation LTRIM_ZERO, já existente)
├── srs/gbak-domain-remap             (este trabalho; base do PR upstream)
└── srs/fb5-custom                    (integradora: merge das duas, build interno)
```

Os conjuntos de arquivos são disjuntos (`src/intl/*` e projetos MSVC de um lado, `src/burp/*` do
outro), então o merge na integradora deve ser limpo. O PR upstream sai de `srs/gbak-domain-remap`
sozinho, sem arrastar a collation.

## 12. Riscos conhecidos e não mitigados

Decisão explícita desta versão: os riscos de 12.2 e 12.3 ficam **documentados, não validados em
código**. O escopo é o build interno, onde o operador conhece o esquema que está migrando. Qualquer
um deles vira validação obrigatória se o alvo mudar. A seção 12.1 é caso à parte: era o risco de
maior impacto levantado em revisão, foi testado e não se sustenta, e fica registrada com o método
para não ser relevantada.

### 12.1 BLR dependente: risco levantado e refutado empiricamente

O gbak restaura BLR **já compilado**, nunca recompila a partir do fonte: `RDB$PROCEDURE_BLR`
(`restore.epp:6991`), `RDB$TRIGGER_BLR` (`9009`, `9155`), `RDB$COMPUTED_BLR` (`5510`),
`RDB$VALIDATION_BLR` (`5586`), `RDB$VIEW_BLR` (`7932`). `RDB$VALID_BLR` também vem do backup intacto
(`7027`, `9216`). Isso levantou a hipótese de que declarações de parâmetro e de variável local
carregariam o tipo antigo embutido no blob, fazendo `'000123'` voltar como `'123'` ao atravessar uma
procedure, justamente onde a collation LTRIM_ZERO deveria proteger.

**A hipótese foi testada e não se confirma.** Método: banco com domínio `TDR_CNPJ AS BIGINT`,
procedure com parâmetro sobre o domínio, segunda procedure com variável local
`DECLARE VARIABLE V TDR_CNPJ`, e um computed field `CNPJ * 2`. Conversão aplicada com
`ALTER DOMAIN TDR_CNPJ TYPE VARCHAR(20) CHARACTER SET ISO8859_1`. Resultados:

- O BLR das procedures ficou **byte a byte idêntico** antes e depois da conversão (mesmo valor de
  `HASH(RDB$PROCEDURE_BLR)`, mesmo `OCTET_LENGTH`). `RDB$VALID_BLR` permaneceu 1.
- `'000123'` atravessou tanto o parâmetro quanto a variável local e voltou `'000123'`.
- O computed field numérico sobre coluna agora textual continuou avaliando, com conversão implícita.

Conclusão: o BLR referencia o domínio, e o tipo é resolvido na execução a partir do catálogo
(`RDB$PROCEDURE_PARAMETERS.RDB$FIELD_SOURCE` aponta para o domínio nomeado). Como o BLR não muda com
a conversão, o estado final produzido pelo gbak (BLR vindo do backup somado a catálogo remapeado) é
equivalente ao estado testado.

O argumento não depende só dessa equivalência. O BLR restaurado é compilado no commit de metadados
(`dfw.epp:1261`, `createRoutine`), e a resolução deste recurso acontece antes da primeira relation,
portanto antes desse commit (seção 4.1). A compilação ocorre contra o catálogo já corrigido por
construção, e não por coincidência de ordenação.

Ressalvas do teste: usou `VARCHAR(20)`, que é o que o DDL aceita (ver 12.2), e não passou pelo gbak
com o remap. A largura não altera o mecanismo verificado. Um computed field ou check numérico sobre
coluna que passe a aceitar não-dígitos falha na avaliação, o que é consequência esperada da
conversão e não do mecanismo de BLR.

### 12.2 Conversão que o próprio engine recusa (resolvido na seção 5.2.1)

**Status: mitigado por padrão.** A regra de largura mínima da seção 5.2.1 adota exatamente o limite
do `checkUpdate`, então o recurso deixa de fazer o que o DDL proíbe, exceto nas regras que pedirem
`UNCHECKED` explicitamente (seção 5.2.2). O texto abaixo fica como registro do problema e do porquê
da regra existir.


`AlterDomainNode::checkUpdate` (`src/dsql/DdlNodes.epp:4441-4442`) calcula, para tipo de origem não
textual, `origLen = DSC_string_length(&origFld.dyn_dsc)`, o que dá 20 para `BIGINT`, e rejeita
`newFld.dyn_charlen < origLen` com `isc_dyn_char_fld_too_small`. Ou seja: `ALTER DOMAIN TDR_CNPJ
TYPE VARCHAR(17)` é recusado pelo DDL. O recurso faz por fora o que o engine impede por dentro, e
pula junto as demais salvaguardas do caminho DDL (bloqueio de identity em `DdlNodes.epp:5160`,
rebuild de índices em `5180`).

No caso concreto que motiva o recurso a largura é suficiente, porque o dado é um CNPJ de 14 dígitos
e o domínio modelo `TDR_CNPJ2` já é `VARCHAR(17)`. O limite de 20 é o pior caso genérico de um
`BIGINT` arbitrário.

### 12.3 Demais riscos

- **Arrays**: domínio com `RDB$DIMENSIONS` passaria a descrever elementos de texto sobre slices
  binárias `int64` (o restore trata array como blob, `restore.epp:12026`). Corrupção real.
- **Identity**: coluna identity sobre domínio convertido para texto é estado que o DDL proíbe
  (`DdlNodes.epp:5160`).
- **Views de expressão**: colunas de expressão, união ou agregado usam domínio implícito `RDB$NNN`,
  não o domínio nomeado, e continuam numéricas. O `SELECT` pela view converte de volta em silêncio.
- **Semântica residual**: `RDB$VALIDATION_BLR` e `RDB$DEFAULT_VALUE` numéricos permanecem no domínio
  convertido e são embutidos no formato (`dfw.epp:6037`).
- **Reentrância**: com "domínio já está no formato alvo" sendo erro fatal, o backup do banco já
  convertido não restaura com a mesma linha de comando.

### 12.3.1 FK remapeada de um lado só: o risco mais caro do grupo

Merece destaque separado porque falha tarde e caro. A criação do índice de FK compara `idx_itype`
com o do parceiro (`dfw.epp:3649` e `idx.cpp:194`), e texto com collation nunca casa com numérico.
A falha é `isc_partner_idx_incompat_type` e acontece na **ativação dos índices, depois do
carregamento completo dos dados**. Em um backup do porte do `scherer_basic_001.fbk`, isso significa
perder horas de restore antes do erro aparecer.

Não é detectável no ponto de resolução: `RDB$RELATION_CONSTRAINTS` ainda não existe naquele momento.
O único lugar onde dá para pegar antes do restore real é o preview com `-m`, já depois do commit de
metadados. Se alguma validação for adicionada apesar da decisão de escopo mínimo, é esta.

### 12.4 Efeito sobre a ambição upstream

A objeção concreta era 12.2: o recurso produziria metadados que o DDL do produto se recusa a criar.
Com a regra de largura mínima da seção 5.2.1, isso deixa de valer no comportamento padrão, e todo
domínio remapeado passa a ser um estado que um `ALTER DOMAIN` alcançaria. O PR volta a ser
defensável.

O `UNCHECKED` da seção 5.2.2 é a parte que um mantenedor questionaria, por ser justamente um escape
da salvaguarda. Se ele for obstáculo na revisão, é removível sem tocar no resto do desenho: é uma
flag na regra e um ramo em `apply_domain_remap`.

Resta a objeção de princípio: o restore deixa de ser fiel ao backup, e `FIX_FSS_*` não é paralelo
perfeito, já que aqueles corrigem rótulo de charset de dados malformados por bug histórico do próprio
gbak, sem mudar tipo nem semântica. Contra-argumento defensável: fazer a mesma conversão via
`ALTER DOMAIN` depois do restore custa uma reescrita completa das tabelas afetadas, enquanto na
janela do restore ela é gratuita, porque o formato ainda nem foi calculado.

Os itens de 12.3 e 12.3.1 continuam sendo o que um revisor cobraria antes de aceitar, em especial a
detecção de FK inconsistente.

## 13. Pontos a confirmar na implementação

- Número livre para `isc_spb_res_fix_domains` e próximos números livres do catálogo de mensagens.
- Comportamento do caminho de Services API para o switch, incluindo o caso `@arquivo` (o arquivo
  precisa existir no servidor, e isso deve gerar erro claro quando não existir).
- Interação com `-PAR`: metadados são processados serialmente e os workers paralelos atuam sobre
  dados (`src/burp/BurpTasks.cpp:889` copia `runtimeODS` do master), então o remap não deve ser
  afetado. Confirmar com um restore `-PAR 5` real.
- Padrão de referência para a resolução: confirmado que existe. `restore.epp:10577-10600` resolve o
  nome do charset do `FIX_FSS_DATA` para id com `FOR ... WITH ... EQ name.c_str()`, handle local e
  `MISC_release_request_silent` no fim. A resolução deste recurso segue a mesma forma.
- Sob `-m`, confirmar quais dos três pontos de resolução são efetivamente alcançados, para garantir
  que o preview aborte com a mesma severidade do restore completo.

# NDP v1 — Nintendo Dev Protocol

Especificação canônica. As implementações (C em `agent/common`, TypeScript em `bridge/packages/core`) devem seguir este documento; os vetores em `docs/protocol/test-vectors/` são gerados por uma terceira implementação independente (`generate.py`) e verificados nas duas.

Palavras **DEVE / NÃO DEVE / PODE** têm o sentido do RFC 2119. Todos os inteiros são **little-endian**. Strings são UTF-8 sem NUL final.

## 1. Transporte
TCP, uma conexão ativa por vez, uma operação por vez. Porta padrão **6464**. O protocolo é um fluxo de *frames*; não há dependência de HTTP/JSON. Um segundo transporte opcional, para a página web servida pelo próprio console, leva **o mesmo fluxo de bytes** dentro de mensagens WebSocket (§15).

## 2. Frame

```
off  size  campo
 0    4    magic         bytes 4E 44 50 31 ("NDP1")
 4    1    version       versão do formato do frame (1)
 5    1    kind          1=REQ 2=RES 3=DATA 4=END 5=ERR 6=EVT
 6    2    flags         bit0 = MAC presente; bit1 = MORE; demais DEVEM ser 0
 8    4    request_id    escolhido pelo Bridge; RES/DATA/END/ERR repetem o id da REQ
12    2    command       ver §5
14    2    status        0 em REQ/DATA/END/EVT; código (§6) em RES/ERR
16    4    payload_len   ≤ max_frame
20    N    payload
20+N  16   mac           só se flags.bit0 (§4)
```

- O cabeçalho tem **20 bytes** e o mesmo layout em todas as versões futuras.
- `max_frame` (limite do **payload**) padrão **65 536**; o agent o anuncia no HELLO. Um receptor DEVE rejeitar (`TOO_LARGE`) frame com `payload_len > max_frame` **sem** alocar/ler o payload.
- Flags desconhecidas (bits 2–15) DEVEM ser 0 no envio; o receptor as trata como `BAD_FRAME`.
- `kind`: **REQ** (Bridge→agent), **RES** (agent→Bridge, sucesso), **ERR** (agent→Bridge, falha), **DATA** (bytes crus de arquivo, qualquer direção), **END** (fim de transferência), **EVT** (agent→Bridge, sem REQ; reservado).
- O frame `HELLO` DEVE sempre ser enviado com `version = 1`, qualquer que seja a versão que o Bridge suporte (bootstrap de negociação).

## 3. Payload TLV
Payload de REQ/RES/ERR/END é uma sequência de campos:
```
[tag u16][len u16][value len bytes]
```
- Campos desconhecidos DEVEM ser ignorados (extensibilidade).
- Tipos por tag (§7): `u8`, `u16`, `u32`, `u64` (little-endian, tamanho exato), `str` (UTF-8), `bytes`. Um campo com tamanho incompatível com o tipo → `BAD_REQUEST`.
- Um mesmo tag repetido: vale o **primeiro**, salvo tags definidas como repetíveis.
- Payload de DATA são bytes crus (sem TLV).

## 4. Autenticação, pareamento e MAC por frame
Sem TLS no console: o canal é **autenticado e íntegro (HMAC por frame), mas não criptografado** (o conteúdo dos arquivos é visível na LAN).

### 4.1 Chave de pareamento
- No console o usuário abre uma **janela de pareamento** (tecla Y; 120 s; no máximo 5 tentativas erradas). O console gera 10 bytes aleatórios seguros (80 bits) e os mostra em Base32 Crockford, `XXXX-XXXX-XXXX-XXXX` (16 caracteres; alfabeto `0123456789ABCDEFGHJKMNPQRSTVWXYZ`; ao ler, `O→0`, `I,L→1`, maiúsculas, hífens/espaços ignorados).
- `PSK = SHA-256("NDP-PSK-v1" ‖ code[10])` (32 bytes). `key_id = SHA-256(PSK)[0:4]`.
- O **código nunca trafega**. O Bridge o digita uma vez (`ndev pair`), deriva a `PSK` e a guarda; o console guarda a `PSK` (até 8 pareamentos) em arquivo que **nenhum comando do protocolo lê ou escreve**.

### 4.2 Provas (HMAC-SHA-256)
`proof(psk, rótulo, cn, dn, extra) = HMAC(psk, rótulo ‖ cn[16] ‖ dn[16] ‖ extra)` onde `cn` = nonce do Bridge (HELLO REQ), `dn` = nonce do console (HELLO RES); `rótulo` = os bytes ASCII `"pair"`, `"auth"` ou `"session"`.

### 4.3 PAIR (0x0004) — só com a janela aberta
`REQ {label str (≤ 15 bytes), proof bytes32 = proof(PSK, "pair", cn, dn, label)}` → `RES {key_id bytes4}`. O console verifica com o código que **está exibindo** (deriva a PSK), guarda `{key_id, PSK, label}`, fecha a janela. Prova errada → `UNAUTHORIZED` e conta uma tentativa (5 → janela fechada e conexão encerrada); janela fechada → `UNAUTHORIZED` ("pairing is not open"); **chaveiro cheio nunca recusa**: um pareamento com o **mesmo rótulo** substitui a chave antiga daquele rótulo (o mesmo navegador pareando de novo), e se ainda assim estiver cheio a **mais antiga** é descartada (o console avisa qual antes de a pessoa aprovar).

### 4.4 AUTH (0x0005)
`REQ {key_id bytes4, proof bytes32 = proof(PSK, "auth", cn, dn, "")}` (frame **sem** MAC) → `RES {}` **já com MAC** (é o primeiro frame selado). Chave desconhecida ou prova errada → `UNAUTHORIZED` (5 falhas encerram a conexão).
`session_key = HMAC(PSK, "session" ‖ cn ‖ dn)`.

### 4.5 Frames selados
Depois de um AUTH bem-sucedido, **todo** frame em **ambas** as direções DEVE ter `flags.MAC`:
```
mac = HMAC-SHA256(session_key, u64le(counter) ‖ header[0:20] ‖ payload)[0:16]
```
(`header` já com o bit MAC ligado). O contador é **por direção**, começa em 0 no primeiro frame selado daquela direção (para o console→Bridge, é o `RES` do AUTH; para o Bridge→console, é o primeiro REQ depois do AUTH) e sobe 1 por frame. Frame sem MAC, MAC inválido ou contador fora de ordem ⇒ a conexão é **encerrada** sem resposta. O comprimento máximo do payload continua `max_frame`; o MAC (16 bytes) vem além dele.

### 4.6 Regras do console
- `auth = "required"` (HELLO RES): antes do AUTH só são aceitos HELLO, PAIR, PAIR_BEGIN/REVEAL/POLL (todos com a janela aberta) e AUTH; qualquer outro comando → `UNAUTHORIZED`. `auth = "none"` (somente testes/desenvolvimento): sem AUTH nem MAC.
- Com `auth = "required"` o HELLO RES traz também `device_id bytes16` (identificador estável do console, gerado no primeiro pareamento), `paired_keys u8` (quantos computadores) e `pairing_open u8`.
- O nonce do console DEVE vir de um gerador seguro; se não houver, com `auth = "required"` o HELLO falha (`IO_ERROR`) — nunca usar nonce fraco.
- Um HELLO **depois** do AUTH → `BAD_REQUEST` ("already authenticated"; abra uma nova conexão). Um frame com o bit MAC **antes** do AUTH encerra a conexão.

### 4.8 Pareamento por comparação de número (agente ≥ 1.3.0)
O jeito de parear **sem digitar o código**: o cliente e o console combinam uma chave por X25519, e a pessoa confere na tela dos dois um **número de 6 dígitos** e aperta **A** no console. A chave **nunca trafega**; quem só escuta a rede não ganha nada, e quem se põe no meio produz um número diferente. O pareamento por código (§4.1–4.3) continua valendo.
Só com a **janela aberta** (tecla Y). Um pedido por vez. Todos os passos são REQ/RES normais, antes do AUTH, na mesma conexão.

1. `PAIR_BEGIN {label (1–15 bytes), pair_commit bytes32}` → `RES {pair_pub bytes32, pair_nonce bytes16}`. O cliente **se compromete** antes de ver a chave do console: `pair_commit = SHA-256("NDP-PAIR2-COMMIT" ‖ pubB ‖ nonceB)` (`pubB` = X25519 público do cliente, `nonceB` = 16 bytes aleatórios). O console gera um segredo X25519 efêmero (`secC`), `pubC = X25519(secC, 9)` e `nonceC` (16 bytes) com o gerador seguro; sem gerador seguro → `IO_ERROR`. Outra conexão com pedido pendente → `BUSY`.
2. `PAIR_REVEAL {pair_pub = pubB, pair_nonce = nonceB}` → `RES {}`. O console confere `SHA-256("NDP-PAIR2-COMMIT" ‖ pubB ‖ nonceB) == pair_commit`; diferente → `UNAUTHORIZED` e o pedido é descartado (**nunca** se mostra número para uma chave trocada depois de ver a do console: sem o compromisso, um atacante poderia gerar chaves até o número coincidir). Depois `shared = X25519(secC, pubB)`; resultado todo zero (ponto de ordem pequena) → `BAD_REQUEST`. O segredo `secC` é apagado.
   `th = SHA-256("NDP-PAIR2-TH" ‖ pubB ‖ pubC ‖ nonceB ‖ nonceC ‖ len(label) u8 ‖ label)`; `k = HMAC(shared, th)`; **`sas = be32(HMAC(k, "sas")[0:4]) mod 10⁶`** (6 dígitos, mostrados `NNN NNN`); `PSK = HMAC(k, "psk")`; `key_id = SHA-256(PSK)[0:4]`. As duas pontas calculam o mesmo. O console mostra o número na tela de cima com o nome e o endereço de quem pediu; o cliente mostra o dele.
3. `PAIR_POLL {}` (a cada ~0,5 s) → `RES {pair_state}`: `0` esperando a pessoa. Se ela aperta **A**: o console guarda `{key_id, PSK, label}`, fecha a janela (uso único) e o próximo POLL devolve `{pair_state = 1, key_id, proof}` **uma única vez**, com `proof = HMAC(PSK, "NDP-PAIR2-OK" ‖ pubB ‖ pubC)`: o cliente DEVE conferir `proof` e `key_id` (prova de que o console derivou a mesma chave) antes de guardar a `PSK`; diferente → abandonar. **B** (ou 60 s sem resposta) → `2`; (o estado `3` existe por compatibilidade: o chaveiro não recusa mais). Cada recusa conta uma tentativa; **5 fecham a janela**.
- O pedido pertence à conexão que o fez: outra conexão não consegue ler nem responder (`BAD_REQUEST`/`BUSY`), e fechar a conexão descarta o pedido.
- O número **não** vai para o log do console.
- Segurança do número: o atacante precisaria que o número dos dois lados coincidisse; com o compromisso, cada tentativa tem chance de 1 em 10⁶, e cada uma exige que a pessoa aperte A num número diferente (ela aperta B).
Vetores: X25519 (RFC 7748 §5.2 e §6.1, pontos de ordem pequena) e derivações completas em `docs/protocol/test-vectors/` (`pair2`), gerados por uma implementação Python independente e verificados no C, no TypeScript (com `node:crypto`) e no JavaScript da página.

### 4.9 PAIR_LIST (0x0009) / PAIR_FORGET (0x000A) — gerenciar pareamentos (agente ≥ 1.5.0)
Depois do AUTH, qualquer chave pareada pode **ver e apagar qualquer pareamento do console**, não só o próprio: o pareamento já dá acesso total de leitura/escrita às pastas liberadas, e o console já permite apagar todos de uma vez com SELECT×2 no próprio aparelho — estender essa mesma confiança para "apagar um por um pelo navegador" não abre nada de novo.

- `PAIR_LIST {}` → `RES` com uma tag `pair_entry` (0x0064) por chave guardada: `key_id bytes4 ‖ label` (label em UTF-8, sem NUL, tamanho pelo próprio TLV). Sem chaveiro configurado → `UNSUPPORTED_COMMAND`.
- `PAIR_FORGET {key_id bytes4}` → `RES {}`. `key_id` ausente/tamanho errado → `BAD_REQUEST`; não encontrado → `NOT_FOUND`; senão remove do chaveiro (compacta o arquivo, mesmo formato) e salva. Uma sessão já autenticada com a chave removida **continua valendo até fechar a conexão** (mesmo comportamento do SELECT×2, que também não derruba sessões ao vivo); uma nova conexão com essa chave falha o AUTH.
- Como todo comando depois do AUTH, ambos exigem um frame selado (MAC por frame, §4.5).

## 5. Comandos
| ID | Nome | M0 | Descrição |
|---|---|---|---|
| 0x0001 | HELLO | ✔ | negocia protocolo, anuncia plataforma |
| 0x0002 | PING | ✔ | eco de nonce |
| 0x0004 | PAIR | M5 ✔ | pareamento (janela aberta no console) |
| 0x0005 | AUTH | M5 ✔ | prova de posse da chave pareada |
| 0x0006 / 0x0007 / 0x0008 | PAIR_BEGIN / PAIR_REVEAL / PAIR_POLL | ✔ (1.3.0) | pareamento por comparação de número (§4.8) |
| 0x0009 / 0x000A | PAIR_LIST / PAIR_FORGET | ✔ (1.5.0) | lista/apaga qualquer pareamento do console (§4.9) |
| 0x0010 | DEVICE_INFO | M2 ✔ | modelo, firmware, RAM, regiões de memória e capacidade do SD |
| 0x0011 | ACCESS_INFO | M5 ✔ | modo e pastas que o dono liberou (só depois do AUTH) |
| 0x0020 | FS_LIST | M3 ✔ | lista um diretório (paginado) |
| 0x0021 | FS_STAT | M3 ✔ | tipo, tamanho, mtime |
| 0x0022 | FS_READ | M4 ✔ | lê arquivo em streaming |
| 0x0030 | FS_WRITE | M5 ✔ | grava um arquivo (temp → validar → rename) |
| 0x0031 | FS_MKDIR | M5 ✔ | cria um diretório |
| 0x0032 | FS_RENAME | ✔ (1.2.0) | move/renomeia um arquivo ou pasta (nunca sobrescreve) |
| 0x0033 | FS_DELETE | M5 ✔ | **move** para a lixeira (nunca apaga de verdade) |
| 0x0035 | FS_COPY | ✔ (1.3.0) | copia UM arquivo dentro do cartão, em passos, feita pelo console |
| 0x0040 | INSTALL_URL | ✔ (1.8.0, só 3DS) | pede ao console que ofereça instalar um título de uma URL; o dono confirma no console |
| 0x0034 | FS_PURGE | ✔ (1.2.2) | apaga **de verdade**, só o que está dentro de uma lixeira (ou esvazia a lixeira) |
| 0x0040+ | LOG_* / CRASH_* | fase 3 | |
| 0x8000–0xFFFF | reservado a extensões de plataforma | | |

Um comando desconhecido → ERR `UNSUPPORTED_COMMAND`. Todo comando exceto HELLO antes de um HELLO bem-sucedido → ERR `HELLO_REQUIRED`. Kind diferente de REQ onde REQ é esperado → ERR `BAD_REQUEST`.

## 6. Status
| Código | Nome | Uso |
|---|---|---|
| 0 | OK | |
| 1 | UNSUPPORTED_PROTOCOL | sem versão em comum |
| 2 | UNAUTHORIZED | falta pairing/AUTH |
| 3 | FORBIDDEN_MODE | operação exige modo maior (ex.: escrita em READ_ONLY) |
| 4 | PROTECTED_PATH | path fora das raízes permitidas ou em zona protegida |
| 5 | NOT_FOUND | |
| 6 | EXISTS | |
| 7 | IO_ERROR | |
| 8 | NO_SPACE | |
| 9 | BAD_REQUEST | campo ausente/inválido |
| 10 | BUSY | outra operação em curso |
| 11 | TOO_LARGE | |
| 12 | HASH_MISMATCH | |
| 13 | TIMEOUT | |
| 14 | UNSUPPORTED_COMMAND | |
| 15 | HELLO_REQUIRED | |
| 16 | BAD_FRAME | frame malformado (magic, flags) |
| 17 | PATH_INVALID | path não normalizável (§10) |

ERR carrega, opcionalmente: `0x0001 detail` (str, texto humano curto, **informativo — clientes NÃO DEVEM depender do texto**; o agent de referência usa exatamente as strings que aparecem nos vetores de diálogo) e `0x0002 os_result` (u32, código nativo do SO/console, para depuração). Sucesso sempre é `kind = RES`, `status = 0`; falha sempre é `kind = ERR`, `status ≠ 0`.

## 7. Tags de campos
| Tag | Nome | Tipo | Onde |
|---|---|---|---|
| 0x0001 | detail | str | ERR |
| 0x0002 | os_result | u32 | ERR |
| 0x0010 | protocol_min / protocol | u16 | HELLO REQ: mínimo; HELLO RES: versão escolhida |
| 0x0011 | protocol_max | u16 | HELLO REQ |
| 0x0012 | bridge_name | str | HELLO REQ |
| 0x0013 | nonce | bytes[16] | HELLO REQ (client_nonce) e RES (device_nonce) |
| 0x0014 | platform | str | HELLO RES (`"3ds"`, `"dsi"`, `"switch"`, `"host"`) |
| 0x0015 | agent_version | str | HELLO RES |
| 0x0016 | auth | str | HELLO RES: `"none"`, `"required"`, `"paired"` |
| 0x0017 | mode | str | HELLO RES: `"READ_ONLY"`, `"DEVELOPMENT"`, `"FULL"` |
| 0x0018 | max_frame | u32 | HELLO RES |
| 0x0019 | supported_min | u16 | ERR de HELLO (`UNSUPPORTED_PROTOCOL`) |
| 0x001A | supported_max | u16 | ERR de HELLO |
| 0x0020 | ping_nonce | u64 | PING REQ e RES (eco) |
| 0x0030 | path | str | FS_* REQ (path de protocolo, §10) |
| 0x0031 | cursor | u32 | FS_LIST REQ: índice da primeira entrada desejada (0 = início) |
| 0x0032 | next_cursor | u32 | FS_LIST RES: cursor da próxima página |
| 0x0033 | entry | bytes | FS_LIST RES, **repetível**: `type u8 ‖ size u64 ‖ name utf-8` (ver §13) |
| 0x0034 | type | u8 | FS_STAT RES: 1 = arquivo, 2 = diretório |
| 0x0035 | size | u64 | FS_STAT RES: bytes |
| 0x0065 | url | utf-8 | INSTALL_URL REQ: URL `http://` ou `https://`, ASCII imprimível sem espaços, até 1023 bytes |
| 0x0036 | mtime | u64 | FS_STAT RES: segundos Unix UTC; **0 = desconhecido** |
| 0x0037 | offset | u64 | FS_READ REQ (padrão 0) |
| 0x0038 | length | u64 | FS_READ REQ (0 = até o fim do arquivo) |
| 0x0039 | chunk | u32 | FS_READ REQ: tamanho de cada DATA (padrão 32 768; limitado a [512, max_frame]) |
| 0x003A | want_hash | u8 | FS_READ REQ: 1 = agent calcula SHA-256 dos bytes enviados |
| 0x003B | sha256 | bytes[32] | FS_READ END (só se want_hash = 1) |
| 0x003C | total_size | u64 | FS_READ RES: tamanho total do arquivo |
| 0x003D | will_send | u64 | FS_READ RES: bytes que virão em DATA |
| 0x003E | list_more | u8 | FS_LIST RES: 1 = pode haver mais entradas (pedir `next_cursor`), 0 = fim |
| 0x0040 | overwrite | u8 | FS_WRITE REQ: 0 = nunca sobrescrever (padrão), 1 = substituir |
| 0x0041 | backup | u8 | FS_WRITE REQ: 1 = manter o arquivo antigo como `<nome>.bak` (só com overwrite = 1) |
| 0x0043 | replaced | u8 | FS_WRITE RES: 1 = havia um arquivo e foi substituído |
| 0x0044 | written | u64 | FS_WRITE RES: bytes gravados |
| 0x0045 | max_chunk | u32 | FS_WRITE RES (pronto): maior payload de DATA aceito |
| 0x0046 | trash_path | str | FS_DELETE RES: onde o item foi parar na lixeira |
| 0x0047 | paired_keys | u8 | HELLO RES (auth required): computadores pareados |
| 0x0048 | pairing_open | u8 | HELLO RES (auth required): 1 = janela de pareamento aberta |
| 0x0049 | device_id | bytes[16] | HELLO RES (auth required) |
| 0x004A | key_id | bytes[4] | AUTH REQ; PAIR RES |
| 0x004B | proof | bytes[32] | PAIR REQ / AUTH REQ |
| 0x004C | label | str | PAIR REQ (≤ 15 bytes) |
| 0x004D | read_root | str | ACCESS_INFO RES, repetido: pasta legível (recursivamente) |
| 0x004E | write_root | str | ACCESS_INFO RES, repetido: pasta gravável (subconjunto de read_root) |
| 0x004F | model | str | DEVICE_INFO RES |
| 0x0050 | firmware | str | DEVICE_INFO RES: versão do sistema ("11.17.0-50U") ou a do kernel |
| 0x0051 | ram_total | u64 | DEVICE_INFO RES: RAM física do modelo (128 ou 256 MiB), bytes |
| 0x0052 / 0x0053 | app_mem_total / app_mem_free | u64 | DEVICE_INFO RES: região de memória do app em primeiro plano (onde o próprio agente roda) |
| 0x0054 / 0x0055 | sys_mem_total / sys_mem_free | u64 | DEVICE_INFO RES: região SYSTEM |
| 0x0056 / 0x0057 | sd_total / sd_free | u64 | DEVICE_INFO RES: cartão SD, bytes |
| 0x0060 / 0x0061 / 0x0062 | pair_commit / pair_pub / pair_nonce | bytes32 / bytes32 / bytes16 | PAIR_BEGIN REQ (commit) e RES (pub, nonce do console); PAIR_REVEAL REQ (pub, nonce do cliente) |
| 0x0063 | pair_state | u8 | PAIR_POLL RES: 0 = esperando a pessoa, 1 = aprovado (traz `key_id` e `proof`), 2 = recusado/expirado, 3 = chaveiro cheio |
| 0x0064 | pair_entry | bytes | PAIR_LIST RES: uma por chave pareada, `key_id bytes4 ‖ label` |
| 0x0058 | new_path | str | FS_RENAME REQ (destino) e RES (destino normalizado) |
| 0x0059 / 0x005A | purged / more | u32 / u8 | FS_PURGE RES: itens (arquivos e pastas) removidos por este pedido; `more` (também FS_COPY RES) = 1 se ainda resta trabalho |
| 0x005B / 0x005C | copied / copy_mode | u64 / u8 | FS_COPY RES: bytes já copiados / REQ: 1 = começar (descarta um temporário antigo), 0 = continuar (padrão), 2 = descartar a cópia parcial |

## 8. Ordem de validação de um frame recebido pelo agent
Depois que o decoder entrega um frame, o agent avalia **nesta ordem** e responde ao primeiro problema:
1. `header.version ≠ 1` → ERR `UNSUPPORTED_PROTOCOL` (com `supported_min/max`);
2. `kind ≠ REQ` → ERR `BAD_REQUEST`;
3. payload não é uma sequência TLV bem formada (tamanho que ultrapassa o payload, ou resto < 4 bytes) → ERR `BAD_REQUEST`;
4. há uma transferência em curso nesta conexão → ERR `BUSY` (o REQ é descartado; a transferência continua);
5. `command = HELLO` → §9;
6. sem HELLO prévio na conexão → ERR `HELLO_REQUIRED`;
7. comando conhecido → seu tratamento; senão → ERR `UNSUPPORTED_COMMAND` (inclui comandos FS quando o agent não tem filesystem).

## 9. Handshake e PING
```
Bridge → HELLO REQ  {protocol_min=1, protocol_max=1, bridge_name, nonce[16]}
Agent  → HELLO RES  {protocol=1, platform, agent_version, nonce[16], auth, mode, max_frame}
```
O agent escolhe a maior versão comum a `[protocol_min, protocol_max]` e às que ele suporta. Sem interseção: `ERR UNSUPPORTED_PROTOCOL` com `supported_min/max` e a conexão permanece aberta (o Bridge decide fechar). `protocol_min > protocol_max` ou campos ausentes → `BAD_REQUEST`. Um segundo HELLO na mesma conexão reinicia a sessão.

`PING REQ {ping_nonce}` → `PING RES {ping_nonce}` (mesmo valor). Ausência de `ping_nonce` → `BAD_REQUEST`.

## 10. Paths
Um path de protocolo é **absoluto**, relativo à raiz do SD (`/3ds/x` ⇒ `sdmc:/3ds/x`), com componentes separados por `/`. **Normalização** (feita pelo agent, de forma autoritativa, e pelo Bridge para falhar rápido) — o path é **rejeitado** (`PATH_INVALID`) se:
1. não começa com `/`, ou é vazio, ou contém NUL, bytes de controle (< 0x20 ou 0x7F), ou `\`;
2. UTF-8 inválido (inclui sequências overlong, surrogates e > U+10FFFF);
3. contém componente vazio (`//`), `.` ou `..`;
4. algum componente contém `: * ? " < > |` (proibidos em FAT), termina em `.` ou espaço, ou contém `~` seguido de dígito (nome curto 8.3 que poderia driblar a política);
5. componente > 255 bytes ou path > 1024 bytes.
Uma `/` final única é removida (exceto na raiz `/`). O resultado normalizado é o que a política e o SO recebem.

**Comparação de prefixo** (raízes/zonas): por **componente**, com dobra de caixa **somente ASCII** (FAT é case-insensitive; para não-ASCII o agent **falha fechado**: só considera igual se os bytes forem idênticos). `/luma` cobre `/luma` e `/luma/x`, **não** `/lumax`.

## 11. Política de acesso
Entrada: `mode` (`READ_ONLY` < `DEVELOPMENT` < `FULL`), `op` (`read`|`write`), path, e a configuração `{read_roots, write_roots, never_read, never_write, write_except}` (listas de paths normalizados; `write_except` é opcional).

Avaliação, na ordem (a primeira que falha encerra):
1. path inválido → `PATH_INVALID`.
2. `op = write` e `mode = READ_ONLY` → `FORBIDDEN_MODE`.
3. path em alguma `never_*` da operação → `PROTECTED_PATH`. **Exceção de escrita:** um path dentro de uma zona `never_write` Z é liberado se existir uma `write_except` E tal que o path está dentro de E e E é sub-pasta **própria** de Z (E ≠ Z). Uma zona mais funda dentro de E protege de novo.
4. path fora de todas as raízes (`read_roots` / `write_roots`) → `PROTECTED_PATH`.
5. caso contrário → `OK`.

Padrões: `read_roots = ["/"]`; `write_roots = ["/3ds/nintendo-dev-agent"]`;
`never_write = ["/Nintendo 3DS", "/luma", "/boot.firm", "/gm9", "/private", "/3ds/nintendo-dev-agent/config"]`;
`never_read = ["/3ds/nintendo-dev-agent/config"]` (política e chaves);
`write_except = ["/luma/plugins", "/luma/titles"]` (plugins 3GX e substituição de arquivos de jogo/layeredfs — o que um desenvolvedor edita no Luma; o resto de `/luma`, como `config.ini`, `payloads` e `sysmodules`, pode impedir o console de ligar e continua protegido).
Um root ou zona pode ser um arquivo (`/boot.firm`). A configuração só é alterada com confirmação física no console (ver `ARCHITECTURE.md`); o protocolo NÃO tem comando para isso.

Os padrões acima são os de `ndp_policy_init_default` (e do agente de teste). **O agente do 3DS não os usa:** ele monta a política a partir da lista do dono (§11.2), cujo padrão é *só a pasta do próprio agente*.

### 11.1 Travessia (navegar até uma pasta liberada)
Se as raízes de leitura estão fundas (`/roms/gba`), o cliente precisa conseguir descer até elas. Por isso `FS_STAT` e `FS_LIST` (e só eles; nunca `FS_READ` nem escrita) aceitam também um diretório que seja **ancestral próprio** de alguma `read_root` e **não** esteja numa zona `never_read`:
- `FS_STAT` responde normalmente (é um diretório).
- `FS_LIST` responde só com as entradas `e` para as quais `visível(e)` vale: `e` está dentro de alguma `read_root` (e fora de `never_read`) **ou** `e` também é ancestral próprio de uma `read_root`. O resto do diretório (arquivos, pastas irmãs, `/luma`…) não aparece. Como a varredura filtrada lê entradas que não mostra (no 3DS cada uma custa ~10 ms), o agente lê no máximo **64 entradas por resposta** (`NDP_TRAVERSAL_SCAN_MAX`); uma resposta pode, então, vir com poucas entradas (até nenhuma) e `list_more = 1` — o cliente segue o `next_cursor` até `list_more = 0`.
- Qualquer outro caminho fora das raízes continua `PROTECTED_PATH`. A travessia não dá acesso ao conteúdo de nada.
Vetores: `traverse` em `vectors.json` (`travessível`/`visível` por configuração e caminho; inclui uma raiz dentro de `never_read`, que nunca serve de caminho).

### 11.2 Lista de pastas do dono
Quem escolhe o que é liberado é a pessoa **no console** (botão A → "Access folders"; nenhum comando do protocolo altera isto). A lista tem até **6 entradas** `{caminho normalizado, nível}`, nível `READ` ou `WRITE` (`WRITE` implica `READ`), e vira a política assim: `read_roots = [/3ds/nintendo-dev-agent] + todas as entradas`; `write_roots = [/3ds/nintendo-dev-agent] + entradas WRITE`; `never_*` = padrões (§11). A pasta do agente é sempre liberada.
- Um nível só pode ser concedido se o caminho não estiver numa zona protegida: `READ` fora de `never_read`, `WRITE` fora de `never_write` (exceto o que `write_except` libera; senão `PROTECTED_PATH`). Conceder `WRITE` em `/` é possível, e as zonas `never_write` continuam valendo dentro dele.
- Ciclo do botão Y numa pasta: nenhum → leitura → leitura+escrita → nenhum, pulando níveis já concedidos por uma pasta-mãe e os proibidos.
- Arquivo `/3ds/nintendo-dev-agent/config/access.bin`: `"NDPA"`, versão 1, `count u8`, 2 bytes zero, depois `count × {nível u8, len u8, caminho}` e o SHA-256 de tudo antes. Qualquer inconsistência (checksum, nível, caminho não canônico, duplicado, zona proibida) ⇒ **lista vazia** (só a pasta do agente): falha fechada. Escrita atômica (`.tmp` → verificar → `.bak` → rename). Vetores: `access` em `vectors.json` (operações, política resultante e os bytes do arquivo).

### 11.3 ACCESS_INFO (0x0011)
`REQ {}` → `RES {mode str, read_root str…, write_root str…}` (só depois do AUTH). Serve para o cliente (e o assistente) saber o que pode tocar, em vez de descobrir por erros `PROTECTED_PATH`.

### 11.4 DEVICE_INFO (0x0010)
`REQ {}` → `RES {model str?, firmware str?, ram_total u64?, app_mem_total/app_mem_free u64?, sys_mem_total/sys_mem_free u64?, sd_total/sd_free u64?}`. Depois do AUTH, em qualquer modo (só leitura). **Um campo só aparece se o console conseguiu medi-lo**: o agente nunca inventa valor (uma resposta vazia é válida). Sem suporte da plataforma → `UNSUPPORTED_COMMAND`; falha ao ler → `IO_ERROR`. No 3DS, ler o espaço livre do SD pode levar alguns segundos num cartão grande (o SO lê a FAT): o cliente usa o timeout longo. Os valores de memória descrevem o momento da consulta. `app_mem_*` é a região do app em primeiro plano; **o agente do 3DS não a informa** (ele toma toda a região ao iniciar, então o "livre" seria sempre 0, e o total seria o modo de memória do próprio CIA, não o que um jogo teria). `sys_mem_*` é informado.

## 12. Fluxo de recepção (normativo para o decoder)
Um decoder DEVE: acumular até 20 bytes; validar `magic`; ler `payload_len`; rejeitar `> max_frame` imediatamente; acumular `payload_len` (+16 se MAC); só então entregar o frame. Em erro de magic/flags a conexão é considerada desincronizada e DEVE ser fechada (não há ressincronização).

## 13. Comandos de filesystem (somente leitura)
Todos exigem HELLO prévio e passam pela política de leitura (§11) com o `path` normalizado. Falhas de política: `PATH_INVALID`, `PROTECTED_PATH`. Path ausente: `BAD_REQUEST`. Inexistente/inacessível como arquivo ou diretório: `NOT_FOUND`. Erro do SO: `IO_ERROR` (+ `os_result` quando houver).

### FS_STAT
`REQ {path}` → `RES {type, size, mtime}`. Só arquivos regulares e diretórios existem para o protocolo (links simbólicos e dispositivos são `NOT_FOUND`).

### FS_LIST
`REQ {path, cursor?}` → `RES {entry…, list_more, next_cursor}`.
- `path` deve ser diretório (senão `BAD_REQUEST`). `.` e `..` nunca aparecem. Nomes com mais de 255 bytes UTF-8 são omitidos (não endereçáveis, §10).
- Cada `entry` = `type u8 (1 arquivo, 2 diretório) ‖ size u64 ‖ name` (nome = o resto do valor, UTF-8, sem NUL). Ordem = a do diretório, estável entre páginas; o Bridge ordena se quiser.
- Uma página traz no máximo 100 entradas. `list_more = 1` significa que o Bridge deve repetir com `cursor = next_cursor`; a última página pode vir vazia com `list_more = 0`. `cursor` além do fim → página vazia, `list_more = 0`.

### FS_READ
`REQ {path, offset?, length?, chunk?, want_hash?}`.
1. Falhas de validação → `ERR` (nenhum dado é enviado): `offset > size` → `BAD_REQUEST`; diretório → `BAD_REQUEST`.
2. Sucesso → `RES {total_size, will_send}` com `will_send = min(length ou ∞, size − offset)`.
3. Depois, o agent envia **N frames `DATA`** (payload = bytes crus do arquivo, ≤ `chunk`; flag `MORE` em todos menos no último) e por fim um frame **`END`** (payload vazio, ou `{sha256}` se `want_hash = 1`). Todos repetem `request_id` e `command` da REQ. `will_send = 0` → RES seguido direto de END.
4. Erro no meio (arquivo encolheu, erro de SD) → um frame `ERR` no lugar do `END`; o Bridge descarta o que recebeu.
5. O Bridge NÃO DEVE enviar REQ antes do `END`/`ERR` da transferência. Se o fizer, o núcleo do agent responde `BUSY` ao REQ (a transferência continua); o servidor de referência (`agent/posix`) simplesmente não lê o socket durante o streaming, então o REQ só é processado depois do fim. Se a conexão cair, a transferência é abortada e o arquivo fechado.
6. `sha256` cobre exatamente os bytes enviados em DATA.

## 14. Comandos de escrita (exigem modo DEVELOPMENT ou FULL)
Passam pela política de **escrita** (§11): `FORBIDDEN_MODE` em `READ_ONLY`; `PROTECTED_PATH` fora das `write_roots` ou dentro de uma zona `never_write`; `PATH_INVALID`. O agent NUNCA cria diretórios implicitamente (o pai deve existir, senão `NOT_FOUND`) e NUNCA apaga nada de forma definitiva por pedido do Bridge (`FS_DELETE` só move para a lixeira).

### FS_MKDIR
`REQ {path}` → `RES {}`. Já existe (arquivo ou diretório) → `EXISTS`.

### FS_WRITE
Sequência (o Bridge espera o `RES` de "pronto" antes de enviar DATA):
```
Bridge → REQ FS_WRITE {path, size, overwrite?, backup?, sha256?}
Agent  → RES {max_chunk}                       (ou ERR: nada foi criado)
Bridge → DATA … DATA (MORE em todos menos no último) → END   (mesmo request_id)
Agent  → RES {written, sha256, replaced}       (ou ERR)
```
- `size` (obrigatório) é o total de bytes. `size = 0` → nenhum DATA, só END. Cada DATA ≤ `max_chunk`.
- `sha256` opcional no REQ = digest esperado; o agent calcula sempre o SHA-256 do que recebeu e o devolve no RES final para o Bridge conferir de ponta a ponta.
- **Atomicidade:** o agent grava em `<path>.ndp-tmp` (mesmo diretório; sufixo reservado — um `path` que termine em `.ndp-tmp`/`.ndp-old`/`.bak` é `BAD_REQUEST`; um temp antigo deixado por queda de energia é removido). Só no `END`, depois de conferir tamanho e hash e de sincronizar o arquivo, ocorre a troca:
  1. destino existe e `overwrite = 0` → `EXISTS` (o temp é apagado);
  2. `overwrite = 1`: o destino vai para `<path>.ndp-old`, o temp vira o destino; se isso falhar o antigo é restaurado; depois o `.ndp-old` é apagado, ou vira `<path>.bak` se `backup = 1`.
- Falha em qualquer ponto → `ERR`, temp apagado, destino intacto. Após um `ERR` no meio, o agent descarta os DATA restantes desse `request_id` até o `END` ou até o próximo REQ.
- `size` ≠ bytes recebidos → `BAD_REQUEST`; `sha256` ≠ calculado → `HASH_MISMATCH`.
- Enquanto o upload está ativo, qualquer outro REQ recebe `BUSY`. Mudar o modo para `READ_ONLY` ou fechar a conexão aborta o upload.

### FS_DELETE (lixeira)
`REQ {path}` → `RES {trash_path}`. **Não remove dados**: o item (arquivo ou diretório inteiro, com o conteúdo) é *movido* por `rename` (atômico, mesma partição) para `<raiz>/.ndp-trash/<nome>`, onde `<raiz>` é a mais específica das `write_roots` que contém o `path`.
- Passa pela política de escrita (§11): `FORBIDDEN_MODE`, `PROTECTED_PATH` (fora das raízes ou em zona `never_write`), `PATH_INVALID`.
- `path` igual a uma `write_root` → `BAD_REQUEST` ("cannot delete a write root"). `path` já dentro de uma lixeira → `BAD_REQUEST` (remoção definitiva não existe no protocolo). Inexistente → `NOT_FOUND`.
- A lixeira (`.ndp-trash`) é criada sob demanda (um `mkdir`, que no 3DS leva ~6 s na primeira vez por raiz). Nome de destino = o nome original; se já existir, `<nome>.1`, `<nome>.2`… (o sufixo `.N` não usa `~`, então continua endereçável).
- `FS_WRITE`/`FS_MKDIR` para dentro de uma lixeira → `PROTECTED_PATH` (a lixeira é gerenciada pelo agent). A leitura funciona normalmente (é como se recupera o conteúdo: ler e/ou o usuário renomear pelo cartão).
- Falha no `rename` → `IO_ERROR`, item intacto no lugar original.
- A remoção definitiva **não** existe em `FS_DELETE`: é o `FS_PURGE` abaixo, restrito ao que já está numa lixeira.

### FS_RENAME (0x0032)
`REQ {path, new_path}` → `RES {new_path}`. Move ou renomeia um arquivo ou uma pasta inteira (com o conteúdo) por `rename` (atômico, mesma partição), **sem nunca sobrescrever**.
- Passa pela política de **escrita** (§11) nas **duas** pontas: `FORBIDDEN_MODE` em `READ_ONLY`; `PROTECTED_PATH` se origem ou destino estão fora das `write_roots` ou numa zona `never_write`; `PATH_INVALID`.
- `PROTECTED_PATH` também para: uma raiz configurada (`read_roots`/`write_roots`) ou a lixeira (`<raiz>/.ndp-trash`) como **origem**, e qualquer **destino dentro de uma lixeira** (itens só entram na lixeira por `FS_DELETE`). **Sair** da lixeira (restaurar) é um `FS_RENAME` normal.
- `NOT_FOUND`: origem inexistente ou pasta de destino inexistente (nunca cria pastas). `EXISTS`: o destino já existe, ou origem = destino. `BAD_REQUEST`: `new_path` ausente ou pasta movida para dentro de si mesma.
- Mudar só a **caixa** do nome (`a` → `A`; o cartão FAT não distingue caixa) é aceito: o agente renomeia em dois passos por um nome temporário `<origem>.ndp-ren` e desfaz se o segundo falhar.
- Uma falha do SO deixa a origem no lugar (`IO_ERROR`).

### FS_PURGE (0x0034, agente ≥ 1.2.2)
`REQ {path}` → `RES {purged, more}`. **Apaga de verdade**, mas **somente dentro de uma lixeira** (`<write root>/.ndp-trash`). `path` é um item dentro da lixeira (arquivo, ou pasta com tudo o que ela contém) ou a própria pasta da lixeira, que é **esvaziada** (a pasta continua existindo).
- Passa pela política de **escrita** (§11): `FORBIDDEN_MODE` em `READ_ONLY`; `PROTECTED_PATH` para qualquer coisa fora de uma lixeira (inclusive uma `write_root`, uma zona `never_write` ou uma pasta parecida como `.ndp-trashy`), `PATH_INVALID`, `BAD_REQUEST` sem `path`. `NOT_FOUND` se o item não existe.
- O trabalho por pedido é **limitado** (16 remoções). `more = 1` significa que ainda resta trabalho: o cliente DEVE repetir o mesmo `REQ` até receber `0`. Nada é lembrado entre pedidos (cada um recomeça do que está no disco), então uma queda de conexão no meio é segura. `purged` conta arquivos e pastas removidos neste pedido.
- Links simbólicos não existem para o protocolo: uma pasta que contém um não pode ser esvaziada (`IO_ERROR`) e o alvo do link nunca é tocado. Falha do SO → `IO_ERROR`, o que restou continua lá. Plataforma sem `remove_dir` → `UNSUPPORTED_COMMAND` para pastas.
- O servidor MCP **não** expõe esta operação (um assistente nunca apaga em definitivo); a CLI (`ndev purge`) e a página web sim.

### INSTALL_URL (0x0040, agente ≥ 1.8.0, só 3DS)
REQ: `url`. O agente valida (http/https, ASCII imprimível, sem espaços) e entrega ao console, que **só aceita enquanto a tela "Receber do navegador" do instalador está aberta** e, antes de baixar qualquer coisa, mostra a origem e o título e pede a confirmação do dono (A). RES vazio = o pedido foi recebido (não que foi instalado). Erros: `UNSUPPORTED_COMMAND` (DSi/outro), `BAD_REQUEST` (URL inválida), `BUSY` com `detail` (tela fechada, ou já há um pedido aguardando confirmação). Vale para conexões autenticadas e, quando o console serve a página sem pareamento, para a página também: a proteção é a tela aberta + a confirmação.

### FS_COPY (0x0035, agente ≥ 1.3.0)
`REQ {path, new_path, copy_mode?}` → `RES {copied, total_size, more}`. Copia **um arquivo** dentro do cartão, **feita pelo console** (os dados não passam pelo cliente), em passos limitados (1 MiB por pedido). O destino só aparece no último passo: o console escreve em `<new_path>.ndp-tmp` e o **renomeia** no fim, então uma falha nunca deixa um destino pela metade. O cliente repete o pedido (com `copy_mode` 0) enquanto `more = 1`; a posição do passo é o tamanho do temporário (nada é guardado entre pedidos). O primeiro pedido DEVE levar `copy_mode = 1` (descarta um temporário de outra cópia, para nunca misturar arquivos); `copy_mode = 2` descarta a cópia parcial (cancelar/erro).
- Política: a origem passa pela política de **leitura**, o destino pela de **escrita** (§11): `FORBIDDEN_MODE`, `PROTECTED_PATH` (inclusive destino dentro de uma lixeira; **sair** da lixeira copiando é permitido), `PATH_INVALID`. `NOT_FOUND` (origem, ou pasta de destino), `EXISTS` (destino existe, ou origem = destino; nunca sobrescreve), `BAD_REQUEST` (origem é pasta: o cliente copia pastas arquivo a arquivo, com `FS_MKDIR`; sufixos reservados `.ndp-tmp`/`.ndp-old`/`.bak` no destino; `copy_mode` inválido), `NO_SPACE`/`IO_ERROR` (a cópia parcial fica até `copy_mode = 2` ou um novo início), `UNSUPPORTED_COMMAND` se a plataforma não sabe anexar a arquivos.
- Um temporário maior que a origem é descartado (`IO_ERROR`). Não há verificação de hash: só o tamanho final é conferido (cópia dentro do mesmo cartão).

## 15. Página web e transporte WebSocket (opcional, agente ≥ 1.2.0)
O agente PODE servir uma página (um único HTML, gzip) e um endpoint WebSocket numa **segunda porta TCP** (o app do 3DS usa **8080**). Nada muda no NDP: cada mensagem WebSocket **binária** carrega bytes do mesmo fluxo de frames das §2–§14 (um frame pode atravessar várias mensagens ou várias frames caberem numa só; a fronteira da mensagem não tem significado). Pareamento, AUTH, MAC por frame, política de acesso e modo valem **exatamente** como na porta NDP.

### 15.1 Clientes
- Porta NDP: **um** cliente NDP puro. Porta web: respostas HTTP curtas (no máximo 3 ao mesmo tempo) e **um** cliente WebSocket. Os dois clientes são **independentes** (cada um tem seu estado de agente e sua autenticação; chaves, modo e política são compartilhados): abrir a página não derruba o servidor MCP e vice-versa.
- Uma nova conexão de um tipo **substitui** a anterior do mesmo tipo. Clientes ociosos são desconectados.

### 15.2 HTTP
Somente `GET` e `HEAD`, sem corpo, com o cabeçalho da requisição ≤ 2048 bytes. Erros: `400` (malformada, ou upgrade inválido), `405` (outro método), `413` (a requisição anuncia corpo), `431` (cabeçalho grande demais ou campo longo demais), `505` (não é HTTP/1.x), `404` (rota desconhecida).
Rotas: `/` e `/index.html` — a página, **sempre** `Content-Encoding: gzip` (o agente não descomprime nem negocia), com `ETag` da versão embutida; `/ws` — o upgrade para WebSocket (`426` com `Upgrade: websocket` se a requisição não pede upgrade); `/favicon.ico` → `204`.
Toda resposta leva `Cache-Control: no-cache`, `X-Content-Type-Options: nosniff`, `Referrer-Policy: no-referrer`, `Connection: close` e a CSP `default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src blob: data:; connect-src ws:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'`.

### 15.3 Defesas contra o navegador
A página roda em `http://`, então é o navegador do dono quem fala com o console; um site malicioso aberto no mesmo navegador NÃO pode usá-lo:
- **`Host`** DEVE ser o endereço IP em que o console aceitou a conexão (a porta não é comparada); ausente ou diferente → `403` (defesa contra *DNS rebinding*). O agente de teste em `127.x` também aceita `localhost`.
- **`Origin`**, quando presente, DEVE ser exatamente `http://` + o valor do `Host` da requisição; qualquer outro → `403`, e o console registra a tentativa (vale para o upgrade do WebSocket).
- Upgrade: `GET /ws` com `Upgrade: websocket`, `Sec-WebSocket-Version: 13` (senão `400` com `Sec-WebSocket-Version: 13`) e `Sec-WebSocket-Key` válido; resposta `101` com `Sec-WebSocket-Accept` (SHA-1 + base64, RFC 6455 §4.2.2). Sem subprotocolos nem extensões. Bytes enviados pelo cliente antes do `101` fecham a conexão.
- Frames WebSocket do cliente: **mascarados**, opcode binário (`0x2`) ou continuação (fragmentação permitida), `ping` respondido com `pong`, `close` respondido e encerrado, **máximo 70000 bytes** por frame. Violações fecham com o código WebSocket adequado: `1002` (bits reservados, sem máscara, opcode desconhecido, controle fragmentado ou > 125 bytes, continuação ou início de mensagem fora de ordem), `1003` (mensagem de texto: só binário é aceito), `1009` (frame grande demais).
- A chave de pareamento fica no armazenamento do navegador (`localStorage`, ou `sessionStorage` se o dono não quiser ser lembrado); só provas HMAC trafegam. Como `http://` não é um contexto seguro (não há `crypto.subtle`), a página traz a sua própria SHA-256/HMAC, verificada contra os mesmos vetores de teste (`vectors.json`).

Os vetores em `docs/protocol/test-vectors/` cobrem o handshake WebSocket, o parser HTTP e o decoder de frames WebSocket.

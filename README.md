# PMWRecomp — Pac-Man World Rally (PC) no navegador

Recompilação estática do `PMR.exe` (Pac-Man World Rally para PC, 2006) para WebAssembly:
x86 → C → wasm. Uma camada própria emula Windows, OpenGL 1.1 (sobre WebGL2), FMOD (WebAudio),
DirectInput (teclado e gamepad) e Winsock. A Winsock vira uma rede local virtual por WebSocket,
o que traz o modo de rede do jogo para a internet, com salas.

**Este repositório não contém o jogo.** Não há arquivos do jogo, nem código gerado a partir
do executável, nem decompilação. Para jogar, cada pessoa usa a **própria cópia** do jogo para PC.

## Jogar (site publicado)

1. Abra o site e clique em **Escolher arquivo .zip**.
2. Envie um .zip da pasta onde o jogo está instalado. No Windows: botão direito na pasta →
   *Enviar para* → *Pasta compactada*. Também dá para usar `python tools/make_game_zip.py PASTA`.
   - O site confere a versão do `PMR.exe` (SHA-256 `a017c5ec…3f4c`).
   - Os arquivos ficam só no navegador (IndexedDB) e nunca são enviados ao servidor.
3. **Jogar sozinho** ou **Jogar online**:
   - Salas públicas ou privadas (código de 6 letras ou link de convite), até 4 jogadores.
   - O dono escolhe o modo (corrida, circuito, 5 modos de batalha), a pista, as CPUs e as opções.
   - A partida começa sozinha para todos.

Use um navegador com JSPI (Chrome ou Edge atuais).

## Compilar

Requisitos: Python 3 com `pefile` e `capstone`, [emsdk](https://emscripten.org) (Emscripten 6),
Node.js 20 ou mais, bash (Git Bash no Windows) e uma cópia instalada do jogo.

```bash
export PMWR_GAME_DIR="/caminho/do/jogo"    # pasta com o PMR.exe
python tools/codegen.py                     # gera gen/*.c a partir do seu PMR.exe
./build.sh                                  # compila para dist/
node server/server.js --game "$PMWR_GAME_DIR"   # http://localhost:8080
```

- Com `--game`, o servidor local entrega os arquivos do jogo direto, o que é útil para
  desenvolver.
- Sem `--game` (ou com `--no-game`), funciona como o site público e pede o .zip a cada jogador.
- `http://localhost:8080/test2.html?n=3` abre 2 a 4 jogos lado a lado para testar o online.

### Estrutura

| Pasta | Conteúdo |
|---|---|
| `tools/` | analisador e gerador de código (x86 → C), script do .zip do jogo |
| `runtime/` | CPU/memória, Win32, OpenGL→WebGL2, FMOD→WebAudio, DirectInput, rede virtual, ponte JS |
| `web/` | página, salas online (`online.js`), armazenamento do jogo (`gamestore.js`) |
| `server/` | servidor Node sem dependências: arquivos do site + relay das salas (`/ws`) |
| `deploy/` | preparação do servidor e publicação |

## Publicar (servidor próprio)

O servidor precisa rodar Node.js continuamente e aceitar WebSocket: uma VPS ou máquina
virtual Linux. Hospedagem compartilhada comum não serve.

1. Copie `deploy/config.example.env` para `deploy/config.env` e preencha o IP, o domínio e a
   chave SSH.
2. Na primeira vez, rode `deploy/deploy.sh --setup`. Ele instala Node.js, Caddy (HTTPS
   automático) e o serviço.
3. Depois de cada mudança, rode `./publish.sh "o que mudou"`. Ele compila, envia ao GitHub e
   atualiza o site.

O `pmr.wasm` só pode ser compilado com uma cópia do jogo, por isso a publicação sai desta
máquina, e não de um CI.

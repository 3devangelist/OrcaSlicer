## English

**This is not the official OrcaSlicer.** It is a test build of a fork of
OrcaSlicer where each plate of a project can have its own printer, process,
filaments, plate settings and physical machine: several printers in one
project, one row of plates per machine. The concept is inspired by the
multiple beds of PrusaSlicer 3.0.

This is a test phase: expect bugs, and keep backups of your projects.

- The app is **orange with a red logo on purpose**, so it is not mistaken for
  the official OrcaSlicer.
- It includes an **MCP server**, which lets AI assistants drive OrcaSlicer.
  Start it with `--mcp` (optional `--mcp-port <port>`).
- No printer, filament or process profiles of anyone are included beyond
  OrcaSlicer's standard system profiles: import or create your own.

**Downloads**

- **Windows x64: portable zip only** (no installer). Unzip it and run
  `orca-slicer.exe`. It keeps its own settings in the `data_dir` folder next to
  the program, separate from an installed OrcaSlicer.
- **Linux x86_64: AppImage.** It uses the same settings folder as an installed
  OrcaSlicer (`~/.config/OrcaSlicer`); to keep them apart, start it with
  `--datadir <folder>`.
- **macOS:** no build; build it from source.

**Problems:** please report them in this fork's GitHub
[issues](https://github.com/3devangelist/OrcaSlicer/issues), not to the official OrcaSlicer.

## Português (PT-PT)

**Esta não é a versão oficial do OrcaSlicer.** É uma versão de testes de um
fork do OrcaSlicer em que cada placa de um projeto pode ter a sua própria
impressora, processo, filamentos, definições de placa e máquina física: várias
impressoras num só projeto, uma fila de placas por máquina. A ideia inspira-se
nas várias bases de impressão (multiple beds) do PrusaSlicer 3.0.

Está em fase de testes: conte com erros e guarde cópias de segurança dos seus
projetos.

- A aplicação é **cor de laranja com o logótipo vermelho de propósito**, para
  não ser confundida com o OrcaSlicer oficial.
- Inclui um **servidor MCP**, que permite a assistentes de IA comandar o
  OrcaSlicer. Arranca com `--mcp` (opcional: `--mcp-port <porta>`).
- Não traz perfis de impressora, filamento ou processo de ninguém além dos
  perfis de sistema normais do OrcaSlicer: importe ou crie os seus.

**Transferências**

- **Windows x64: apenas o zip portátil** (sem instalador). Descompacte-o e
  execute o `orca-slicer.exe`. Guarda as suas definições na pasta `data_dir`,
  ao lado do programa, separadas de um OrcaSlicer instalado.
- **Linux x86_64: AppImage.** Usa a mesma pasta de definições de um OrcaSlicer
  instalado (`~/.config/OrcaSlicer`); para as manter separadas, arranque-o com
  `--datadir <pasta>`.
- **macOS:** sem versão compilada; compile-o a partir do código-fonte.

**Problemas:** comunique-os nas [issues](https://github.com/3devangelist/OrcaSlicer/issues) do GitHub deste fork, e
não ao OrcaSlicer oficial.

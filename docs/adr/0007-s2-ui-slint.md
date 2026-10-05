# ADR-0007 — Spike S2: UI de configurações com Slint

- **Status:** aceito — **Slint permanece** (D2); WinUI 3 não é necessário
- **Data:** 2026-10-05
- **Spike:** `spikes/s2-ui/` (galeria com barra lateral e 500 miniaturas); SDK Slint 1.18.1 obtido por `scripts/fetch-deps.ps1` (SHA-256 fixado, instalador **extraído**, nunca executado)

## Critérios (definidos antes, doc 04)

p95 de quadro ≤ 16 ms rolando · início a quente ≤ 600 ms · RAM ≤ 120 MB · Narrador lê a galeria. Falhou algum → WinUI 3.

## Resultados (host: Ryzen 5 5500 + RX 6600, build release)

| Renderizador (`SLINT_BACKEND`) | RAM parada | RAM após rolar 500 itens | Até o loop de eventos | Observação |
|---|---|---|---|---|
| **`winit-software`** | **10,0 MB** | **17,6 MB** | **65 ms** | **~123 quadros/s rolando** (≈ 8 ms/quadro) |
| `winit-skia-software` | 13,1 MB | 21,4 MB | 79 ms | ~122 quadros/s |
| `winit-femtovg` (OpenGL) | 137 MB | 152 MB | 592 ms | |
| `winit-skia` (GPU/wgpu, padrão novo) | 275 MB | 287 MB | 849 ms | |

- **Acessibilidade**: UI Automation enxerga 81 elementos, 59 com nome — inclusive as 36 miniaturas visíveis ("Wallpaper N") e os itens da navegação → o Narrador consegue ler.
- **Windows limpo (Sandbox)** com runtime app-local: 10,2 / 17,8 MB, mesma acessibilidade, 482 ms a frio numa VM nova.
- **Aparência** (captura de tela conferida): tema escuro Fluent, barra lateral com indicador, cantos arredondados, tipografia Segoe UI, acentuação correta. A barra de título ainda é clara — resolver com `DWMWA_USE_IMMERSIVE_DARK_MODE` no HWND (`win32_hwnd()`).
- A diferença entre renderizadores confirma a pesquisa: **o renderizador precisa ser fixado explicitamente**; o padrão novo (GPU) custaria ~16× mais RAM.

## Decisões

1. **Slint** para o `rmw-settings` (M7), com **renderizador por software fixado pelo próprio app** (`SLINT_BACKEND=winit-software` definido antes de criar a janela) e teste de orçamento no CI.
2. **Distribuição**: `slint_cpp.dll` + **runtime VC++ app-local** (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`) ao lado do `rmw-settings.exe`, que usa `/DEPENDENTLOADFLAG:0xA00` (propriedade `RMW_APP_DIR_DLLS`). Os demais executáveis continuam `0x800` e CRT estático. Nenhum objeto/memória do CRT cruza a fronteira com o `slint_cpp.dll`.
3. **Código gerado pelo Slint** compila sem a política de warnings do projeto (`rmw_relax_generated_sources`); nosso código no mesmo alvo mantém `/W4 /WX`.
4. **Galeria virtualizada**: modelo próprio que gera as linhas sob demanda com cache LRU (o que manteve a RAM em ~18 MB com 500 itens) — padrão para o M7.
5. **Licenças**: o pacote inclui `LICENSE.md` e `THIRDPARTY.md` do Slint (uso sob GPLv3, compatível com D1).

## Riscos aceitos

- `slint_cpp.dll` (Rust) **não** é compilado com CFG/CET: o processo de configurações tem mitigação menor que o core. Aceito porque ele é aberto sob demanda, não processa entrada não confiável sem passar pelo core e encerra ao fechar.
- Dependência de um fornecedor (SixtyFPS GmbH) e mudanças de padrão entre versões → versão e hash fixados em `scripts/fetch-deps.ps1`; a UI é cliente IPC e pode ser trocada (doc 07).

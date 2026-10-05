# ADR-0006 — Spike S1: wallpaper em vídeo (Media Foundation + D3D11 + DirectComposition)

- **Status:** aceito — **vídeo permanece na v1.0** (D4)
- **Data:** 2026-10-05
- **Spike:** `spikes/s1-video/` — modo `host` (janela comum, no host) e modo `desktop` (encaixe atrás dos ícones, no Windows Sandbox). Build **release**.

## Perguntas

1. O encaixe atrás dos ícones funciona no layout novo (24H2+ "raised desktop") com DirectComposition?
2. Quanto custa tocar 1080p30 H.264 com Media Foundation + D3D11? Critério de "não seguir": > 150 MB ou > 5% de CPU.
3. Pausar zera o custo?

## Método

- Vídeo de teste gerado localmente com o codificador H.264 do Windows (`IMFSinkWriter`): 1920×1080, 30 fps, 10 s, 8 Mb/s, com um bloco magenta fixo para verificação na tela.
- Reprodução: dispositivo D3D11 com suporte a vídeo + `IMFDXGIDeviceManager` → `IMFMediaEngine` em modo *frame server* (mudo, em loop) → `TransferVideoFrame` para um swapchain `CreateSwapChainForComposition` (flip) → `IDCompositionDevice::CreateTargetForHwnd`. Thread de renderização separada, ritmada por `WaitForVBlank`, apresentando **só quando há quadro novo**.
- Métricas do próprio processo: *Private Bytes* e tempo de CPU (`GetProcessTimes`).

## Resultados

| | Host (RX 6600, monitor 1920×1080, janela comum atrás das outras) | Windows Sandbox (GPU-PV na mesma RX 6600, tela 1520×823, **encaixado atrás dos ícones**) |
|---|---|---|
| Encaixe (layout "raised", filho layered do `Progman` logo abaixo do `SHELLDLL_DefView`) | — | ✅ pai correto, logo abaixo dos ícones |
| Vídeo visível na tela | — | ✅ pixel de teste mudou do wallpaper `#A4BED7` para `#FF01FF` |
| `CanPlayType("video/mp4; codecs=avc1")` | ✅ | ✅ |
| Quadros apresentados | 30,0 fps | 29,9 fps |
| **CPU tocando** | **0,18–0,24%** | **0,02%** |
| **Private Bytes tocando** | **105,6–106,7 MB** | **54,0 MB** |
| Pausado | 0,00–0,03% CPU, 0 quadros | 0,02% CPU, 0 quadros |

Decomposição da memória no host: processo vazio 1,8 MB → **+23,4 MB** ao criar o dispositivo D3D11 (driver AMD) → +0,2 MB swapchain/DComp → +0,4 MB media engine → **+71 MB ao começar a tocar** (pipeline/decodificador). No Sandbox a mesma etapa custou +21 MB.

## Decisão

1. **Critério de "não seguir" atendido** (pior caso 106,7 MB < 150 MB; CPU 0,24% < 5%) → **o vídeo fica na v1.0**.
2. A **meta de 90 MB** (doc 01) **não foi atingida no host** (+17 MB). A meta **não** é relaxada: vira tarefa do M6:
   - trocar `IMFMediaEngine` por `IMFSourceReader` (DXVA, `MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT` baixo) + D3D11 Video Processor, controlando o número de buffers;
   - entender a diferença host × Sandbox (+71 MB × +21 MB) — candidatos: escalonamento 1080p→1080p vs. reduzido, MFT de decodificação diferente (fabricante × Microsoft), janela top-level × filha;
   - liberar o pipeline após 2 min pausado (já planejado).
   Se o M6 não chegar a 90 MB, a meta volta à mesa com o dono do projeto.
3. Arquitetura validada para o M6: **DirectComposition** como caminho primário, janela filha *layered* do `Progman` no layout novo, thread de janela só com mensagens, renderização em thread própria, `WM_NCHITTEST → HTTRANSPARENT`.

## A confirmar

- Layout antigo (Win10 / Win11 ≤ 23H2): sem ambiente disponível (melhor esforço, D3).
- 4K e 60 fps; multi-monitor; HDR — no M6.
- Encaixe no host real (desktop do usuário) — no teste real do M6.

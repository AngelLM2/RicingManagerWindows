# ADR-0004 — Spike S5: identidade de monitor e fidelidade do snapshot de wallpaper

- **Status:** aceito
- **Data:** 2026-10-05
- **Spike:** `spikes/s5-monitors-snapshot/` — parte "monitors" no host (somente leitura), parte "snapshot" no Windows Sandbox

## Perguntas

1. Como identificar um monitor de forma estável (reconexão em outra porta, dock, monitores iguais)?
2. O snapshot consegue restaurar o wallpaper original, inclusive quando ele é o `TranscodedWallpaper`, e o slideshow nativo?

## Resultados

### Identidade

| Máquina | Caminho `IDesktopWallpaper` = `QueryDisplayConfig.monitorDevicePath` | EDID |
|---|---|---|
| Host (1 monitor) | ✅ idênticos | `JRY-2380`, **número de série genérico `1`**, sem descritor de série, nome "DP" (o "nome amigável" também é só "DP") |
| Windows Sandbox | ✅ idênticos | **sem EDID** (`Default_Monitor`) |

Conclusões: EDID nem sempre existe, e quando existe o número de série pode ser genérico (dois monitores iguais colidiriam); o nome amigável pode ser inútil ("DP").

### Snapshot (Windows Sandbox, 25H2)

| Teste | Resultado |
|---|---|
| Capturar wallpaper por monitor, posição, cor de fundo, estado do slideshow | ✅ (`BackgroundType` ausente num perfil novo → tratar ausência como "imagem") |
| `TranscodedWallpaper` é reescrito pelo próximo `SetWallpaper` | ✅ confirmado → **copiar para o Store antes de qualquer mudança** |
| Restaurar a imagem a partir da cópia no Store | ✅ `GetWallpaper` passa a apontar para a cópia |
| Slideshow: capturar itens (`GetSlideshow`), opções e intervalo; trocar para imagem estática; restaurar | ✅ estado `DSS_SLIDESHOW` volta, opções e intervalo idênticos (o critério automático olhou o bit errado, `DSS_ENABLED`; os valores brutos confirmam o sucesso) |
| Windows Spotlight | não testável (Sandbox sem rede) |

## Decisões

1. **Chave de identidade do monitor** (doc 03 §6), em ordem:
   1. EDID completo (fabricante + produto + número de série + série textual) **se for único** entre os monitores conectados e o número de série não for genérico (0 ou 1);
   2. senão, EDID (fabricante + produto) **+ instância do caminho do dispositivo** (`UID…`) e conector;
   3. sem EDID: o caminho do dispositivo inteiro.
   Ao reconectar, casa-se pela chave mais forte disponível; empate → posição relativa (`left`/`right`…) e, por fim, pergunta ao usuário na UI.
2. **Nome exibido**: nome do EDID → nome amigável → "Monitor N (L×A)" — nunca só "DP".
3. **Snapshot do wallpaper**: por monitor, o arquivo exibido é **copiado para o Store** (doc 03 §5.3) antes de qualquer alteração (se o arquivo de origem não existir mais, copia-se o `TranscodedWallpaper`); mais posição, cor de fundo, `BackgroundType` e, se houver slideshow, itens + opções + intervalo. Tudo isso é `Fidelity::Exact`.
4. **Spotlight** (`BackgroundType = 3`): `Fidelity::Lossy` — restaurar grava o tipo de volta e avisa que o Windows pode levar um tempo (ou precisar de rede) para voltar a exibir o Spotlight.
5. Os testes de estado do slideshow usam `DSS_SLIDESHOW` (0x2), não `DSS_ENABLED`.

## A confirmar no teste real (fim do M3)

- Reconectar o monitor em outra porta / dock e confirmar que a chave casa.
- Dois monitores idênticos (se houver na máquina de testes).

# 05 — Roadmap pós-MVP e escalabilidade

O MVP (doc 04) entrega: motor transacional, wallpaper estático e em vídeo, aparência, cursores, **barra própria totalmente dimensionável com bandeja**, **sistema de plugins da comunidade** (Luau em sandbox), UI de configurações e pacotes. Daqui em diante, cada fase adiciona módulos e **pontos de extensão** sobre o mesmo contrato (`IModule` + API de plugins, doc 12), com os mesmos princípios: reversível, isolado, medido, extensível.

## Visão geral

```
Fase 0       ──► MVP v1.0                ──► v1.1          ──► Fase 2             ──► Fase 3              ──► Fase 4
Fundação         motor, wallpaper             GIF/WebP,         rice completo:          avançado:               distribuição:
+ 9 spikes       estático + vídeo,            TranslucentTB,    paleta automática,      Modo avançado (admin),  Store, ARM64,
                 aparência, cursores,         portátil,         bordas, ícones/fontes,  ícones do sistema,      builds reproduzíveis,
                 BARRA PRÓPRIA + BANDEJA,     dumps WER,        widgets de desktop,     .msstyles experimental, atualização opt-in
                 PLUGINS (Luau),              padrão de         shaders, Windhawk,      plugins nativos,
                 UI, pacotes, 6 rices         fábrica           sons, tela de bloqueio  galeria online
```

---

## v1.1 — o que ficou de fora do MVP de propósito

| Item | Por que saiu da v1.0 |
|---|---|
| **GIF / WebP animado** | o WIC entrega quadros crus: composição e *disposal* do GIF precisam ser escritos à mão; v1.0 recomenda converter para MP4 |
| **Integração TranslucentTB** (botão "Instalar" + configurar pela paleta) | com a barra própria, só faz sentido para quem prefere manter a barra **nativa**; doc 10 §3 |
| **Modo portátil** | conflita com entrada de inicialização, atalhos e cursores apontando para pendrive; FAT/exFAT não garantem escrita atômica |
| **Captura de *fast-fail* via WER** (`WerRegisterRuntimeExceptionModule`) | DLL roda dentro do `WerFault.exe` e a chave pode ser sinalizada por EDR (doc 06 §7) |
| **"Padrão de fábrica do Windows"** | além do "estado original", restaurar valores de uma instalação limpa |
| **Verificação de atualização opt-in** | v1.0 não acessa rede (exceto plugins com permissão de rede aprovada) |

## Fase 2 — "Rice completo" (v1.x)

| Feature | Como | Ponto de extensão (doc 12) | Risco |
|---|---|---|---|
| **Paleta automática do wallpaper** ("pywal para Windows") | cores dominantes (k-means em miniatura) → paleta clara/escura com contraste AA → destaque, bordas, barra, widgets | `palette.generator` | baixo |
| **Widgets de área de trabalho** (estilo Rainmeter) | mesma engine e mesmo host de plugins da barra, janelas no nível do desktop | `desktop.widget` | médio |
| **Wallpaper de shader** | HLSL estilo Shadertoy no `rmw-wallpaper` (tempo, mouse, áudio opcional), watchdog de TDR | `wallpaper.shader` (P3) | baixo |
| **Fontes de wallpaper** | pastas inteligentes, rotação por regra; online só com permissão de rede | `wallpaper.source` | baixo |
| **Transições** | crossfade/slide quando o renderizador está ativo | — | baixo |
| **Bordas e cantos de janela** | `DwmSetWindowAttribute` entre processos, sem injeção (não documentado, funciona empiricamente); reaplicar a cada troca de foco; janelas elevadas não aceitam | módulo + tokens de estilo | médio |
| Bordas avançadas (espessura, gradiente, animação) | janelas *overlay* Direct2D/DirectComposition | — | médio |
| **Mica/Acrylic em apps** | `DWMWA_SYSTEMBACKDROP_TYPE` em janelas de terceiros, opt-in por app, lista de exclusão | — | médio |
| **Ícones por usuário** | HKCU (tipos de arquivo, Este Computador, Lixeira, unidades, pastas), sem admin (doc 10 §2) | `asset-pack` | baixo |
| **Fontes por usuário** | instalar fontes do rice; títulos/menus Win32 via `SPI_SETNONCLIENTMETRICS` (doc 10 §2) | `asset-pack` | baixo |
| **Integração Windhawk** | botão "Instalar" (o instalador dele pede admin) + YAML gerado da paleta; CLI quando o 2.x estabilizar (doc 10 §3) | `integration` | médio (injeção é do Windhawk) |
| **Sons do sistema** | `HKCU\AppEvents\Schemes` | `asset-pack` | baixo |
| **Tela de bloqueio** | API WinRT `LockScreen` (validar persistência no 24H2); nunca `PersonalizationCSP` | — | baixo/médio |
| **Ajustes do Explorer** | menu de contexto clássico, ícones da área de trabalho etc. — distribuídos como **plugins `tweak`** oficiais | `tweak` (P1) | baixo |
| **Lançador** (estilo PowerToys Run/Flow) | processo próprio; resultados extensíveis por plugins | `launcher.provider` (novo) | baixo |
| **Wallpaper por desktop virtual** | COM não documentado, só em builds conhecidos | — | médio |
| **Renderizador endurecido** | `MitigationOptIn` + ACG após inicializar D3D/MF (doc 06 §5) | — | baixo |
| **API IPC pública** | JSON-RPC estável e documentado para integrações externas (Stream Deck, scripts, outras UIs) | — | baixo |

## Fase 3 — "Avançado" (v2.x)

| Feature | Como | Risco |
|---|---|---|
| **Modo avançado (admin)** | componente opcional "Ferramentas avançadas" em `%ProgramFiles%` com `rmw-elevated.exe` (UAC por operação, lista fechada de comandos) — doc 10 §4 | médio |
| Ícones do sistema inteiro | `HKLM\…\Shell Icons` pelo ajudante elevado | médio |
| **Temas visuais completos (`.msstyles`)** — experimental | ajudante copia para `%WinDir%\Resources\Themes`; liberar temas não assinados exige Windhawk `uxtheme-hook` (ou SecureUxTheme) instalado por escolha do usuário; ponto de restauração + recuperação explicados antes (doc 10 §4.3) | **alto** (loop de login) |
| **Plugins nativos (P4)** e *providers* nativos | `rmw-pluginhost-native` (Low IL, Job), só IPC, permissões aprovadas | médio |
| **Galeria online** de rices e plugins | só índice (pacotes nos repositórios dos autores), hash, permissões exibidas, política de conteúdo (licença declarada, selo "Comunidade · não oficial", aviso de marca, canal de remoção — doc 11 §3) | baixo/médio |
| Assinatura de plugins pelo autor | opcional, exibida na instalação | baixo |

## Fase 4 — "Distribuição" (v3.x)

| Feature | Como |
|---|---|
| Microsoft Store | MSIX com `unvirtualizedResources` para os ajustes em HKCU |
| ARM64 | build nativo |
| Builds reproduzíveis | `/Brepro`, toolset fixo, verificação independente |
| Atualização automática opt-in | manifesto assinado |

**Fora do roadmap:** substituição da fonte do sistema (`FontSubstitutes`), patch de arquivos do sistema (UltraUXThemePatcher, troca de `imageres.dll`/`.mun`), injeção a partir do nosso binário.

---

## Escalabilidade — como o projeto cresce sem virar bagunça

### Técnica

1. **Tudo é ponto de extensão** (doc 12): cada feature nasce com *providers*, ações, esquema de configuração e tokens de estilo — e nossas próprias features usam a mesma API que a comunidade.
2. **Contrato único de módulo** (`IModule`) para o que altera o sistema → nova feature = novo módulo + teste de contrato; o motor não muda.
3. **Camada de compatibilidade de versão do Windows** (`platform/WindowsVersion`, `Capabilities`): a UI só mostra o que funciona naquela máquina; quando uma atualização do Windows quebra algo, o conserto fica num lugar só.
4. **Formatos versionados** (`schema = N` no rice e no `plugin.toml`, `api = "^1.0"` nos plugins) com migração automática.
5. **API IPC versionada** com handshake → UIs e integrações sem acoplamento ao código interno.
6. **Workers por processo** → features pesadas e código de terceiros entram como processos supervisionados, sem aumentar o risco do core e da barra.
7. **Orçamentos no CI** → regressão de memória quebra o build.

### Projeto (open source)

1. **ADRs** (`docs/adr/`) para toda decisão relevante.
2. Issues com rótulos `good first issue`, `module:*`, `plugin-api`, `windows-version:*`, `security`.
3. **Contribuição de baixo atrito**: rices (designers) e plugins Luau (scripters) sem precisar mexer no C++.
4. Repositórios separados: `rmw-rices` (oficiais), `rmw-plugin-template`, exemplos de plugins.
5. **Tradução** por arquivos `.po`.
6. **SECURITY.md** com canal privado de reporte.
7. Releases previsíveis (a cada 4–6 semanas) com CHANGELOG automático.

# 10 — Modo avançado (admin) e integrações com ferramentas de injeção

> Pedido do dono do projeto: temas visuais completos, ícones etc. **podem pedir admin**, e ferramentas que injetam no Explorer (TranslucentTB e afins) **podem ser instaladas** — mas **só se a pessoa quiser, clicando explicitamente**, e com tudo muito bem explicado.
>
> Como isso convive com os princípios de segurança (doc 06): **o nosso app continua nunca injetando código e nunca patchando arquivos do sistema.** Quem injeta é uma ferramenta de terceiros, conhecida, que o usuário escolhe instalar; quem pede admin é um ajudante mínimo, separado e opcional. Fontes: doc 09 §9.

## 1. Níveis de risco (o que o usuário vê)

| Nível | Nome na UI | O que libera | Admin? | Injeção? | Como ativa |
|---|---|---|---|---|---|
| **0** | Normal | tudo do MVP + (Fase 2) **ícones por usuário** e **fontes por usuário** | não | não | padrão |
| **1** | Integrações | instalar e configurar **TranslucentTB** (barra transparente/blur/acrylic no Win11) e **Windhawk** (estilizar barra, Iniciar, Explorer) | só o instalador do Windhawk pede, com o UAC dele | **sim, pela ferramenta de terceiros** | botão "Instalar" por ferramenta, após tela de explicação |
| **2** | Modo avançado | **ícones do sistema inteiro** e **instalar temas visuais completos (`.msstyles`)** | **sim, UAC a cada operação** | só se o usuário também instalar uma ferramenta que libere temas não assinados | instalar o componente opcional "Ferramentas avançadas" + tela de explicação + confirmação por operação |
| **—** | Nunca | patchar arquivos do sistema (UltraUXThemePatcher, troca de `imageres.dll`/`.mun`), `FontSubstitutes` | — | — | não existe no app (no máximo um link explicando por que não) |

### Tela de consentimento (para cada item dos níveis 1 e 2)

Antes de qualquer botão "Instalar"/"Ativar" funcionar, o usuário vê um cartão com:
1. **O que é** e quem mantém (link para o projeto, licença).
2. **O que ele faz no seu Windows** — em linguagem simples (ex.: "o TranslucentTB coloca um pedaço de código dentro do Explorer para pintar a barra").
3. **O que pode dar errado**, sem eufemismo (ex.: "o Explorer pode reiniciar"; para temas não assinados: "**tela preta / loop no login**").
4. **Como desfazer** — passo a passo, incluindo o caminho pelo Modo de Segurança / WinRE quando for o caso.
5. Caixa **"Li e entendi os riscos"** + botão. A escolha fica no histórico e pode ser desfeita a qualquer momento.

O **modo seguro** do nosso app (doc 06 §7) desativa as integrações e o modo avançado que dependem dele.

## 2. Nível 0 — o que dá sem admin (boa notícia)

Quase tudo de ícones e fontes funciona **por usuário**:

| Item | Mecanismo | Fase |
|---|---|---|
| Ícones de tipos de arquivo | `HKCU\Software\Classes\<ProgID efetivo>\DefaultIcon` + `SHChangeNotify(SHCNE_ASSOCCHANGED)` (apps empacotados podem ignorar) | 2 |
| Este Computador, Lixeira (vazia/cheia) e outros itens da área de trabalho | `HKCU\…\Explorer\CLSID\{…}\DefaultIcon` | 2 |
| Ícones de unidades | `HKCU\Software\Classes\Applications\Explorer.exe\Drives\<letra>\DefaultIcon` | 2 |
| Ícone de pastas específicas | `desktop.ini` + `PathMakeSystemFolder` (uma pasta por vez) | 2 |
| Atualizar cache de ícones | `ie4uinit.exe -show` | 2 |
| Instalar fontes do rice | por usuário: `%LOCALAPPDATA%\Microsoft\Windows\Fonts` + `HKCU\…\Fonts` + `AddFontResourceEx` + `WM_FONTCHANGE` | 2 |
| Fonte de títulos de janela, menus e caixas de mensagem (Win32) | `SystemParametersInfo(SPI_SETNONCLIENTMETRICS / SPI_SETICONTITLELOGFONT)` — reversível, sem admin (Iniciar/Configurações/barra continuam em Segoe UI Variable) | 2 |
| Aplicar/voltar `.theme` com estilos da Microsoft | `.theme` em `%LOCALAPPDATA%\Microsoft\Windows\Themes`; voltar para `aero.theme`/`dark.theme` | MVP (já usado no módulo de aparência) |

Tudo entra como módulos normais (`IModule`), com snapshot e desfazer.

## 3. Nível 1 — integrações (ferramentas de terceiros que injetam)

Regra: **não empacotamos** nem baixamos binários por conta própria. Instalamos pelo **winget** (pacotes com hash verificado) ou pela **Microsoft Store**, em janela visível, **somente após o clique** — e cada ferramenta mostra seus próprios avisos/UAC. Nada silencioso.

### TranslucentTB — candidato a v1.1

| | |
|---|---|
| O que faz | barra de tarefas transparente/blur/acrylic/opaca por estado (desktop, janela maximizada, Iniciar aberto, bateria…) |
| Licença / manutenção | GPLv3, ativo (2026.2, ago/2026) |
| Instalação | Microsoft Store `9PF4KZ2VN4W9` (`ms-windows-store://pdp/?ProductId=9PF4KZ2VN4W9`) ou `winget install --id CharlesMilette.TranslucentTB -e`; MSIX por usuário, **sem admin** |
| Como configuramos | escrevendo o `settings.json` dele (esquema público, recarrega sozinho em ~200 ms) em `%LOCALAPPDATA%\Packages\28017CharlesMilette.TranslucentTB_v826wp6bftszj\RoamingState\` |
| Cuidados | validar contra o esquema; **preservar chaves desconhecidas**; escrita atômica (se o JSON ficar inválido, ele volta silenciosamente ao padrão); snapshot do arquivo anterior para desfazer. Não é uma API oficial — tratamos como integração "melhor esforço" |
| No rice | seção `[integrations.translucenttb]` com as cores/efeitos por estado — derivadas da paleta do rice |

### Windhawk — Fase 2

| | |
|---|---|
| O que faz | plataforma de *mods*: "Taskbar Styler" (~40 temas prontos), "Start Menu Styler", estilos do Explorer etc. |
| Licença / risco | GPL-3.0; injeta em vários processos; teve o certificado revogado pela Microsoft em out/2025 (v1.7 reassinada por outro fornecedor) |
| Instalação | `winget install --id RamenSoftware.Windhawk -e` — o instalador **pede admin** (UAC dele) |
| Como configuramos | **1.x**: não há API — abrimos a página do mod e oferecemos o YAML de configuração (gerado da paleta do rice) para o usuário colar; **não** escrevemos no `HKLM\SOFTWARE\Windhawk` (não documentado e exige admin). **2.x** (hoje em alfa): usar `windhawk-cli` / links `windhawk://mods/<id>` quando estiver estável |

### Fluxo do botão "Instalar"

1. Tela de consentimento (§1).
2. Verifica se o winget existe (`winget --version`); se não, oferece a página da Store ou o GitHub do projeto.
3. Executa `winget install --id <ID> -e --source winget` (ou `msstore`) **numa janela visível**; nunca `--silent` para instaladores que elevam; nunca elevamos o winget nós mesmos. Aceite de termos só depois que nossa tela os mostrou.
4. Detecta a instalação, oferece configurar a partir do rice ativo.
5. "Desinstalar" mostra como remover (winget/Configurações) e desfaz nossa configuração.

## 4. Nível 2 — Modo avançado (admin)

### 4.1 O ajudante elevado (`rmw-elevated.exe`)

| Regra | Por quê |
|---|---|
| **Componente opcional instalado por máquina** em `%ProgramFiles%\RicingManagerWindows\` (instalador separado, que pede admin uma vez) | se o executável elevado ficasse numa pasta do usuário, um malware rodando como usuário poderia trocá-lo e ganhar admin pelo nosso UAC |
| **Um processo por operação**, via `ShellExecuteEx` + `runas` → UAC **toda vez** → faz uma coisa e sai | sem serviço, sem servidor COM elevado, sem processo elevado esperando comandos |
| **Lista fechada de comandos**: `install-theme <id> <sha256>`, `uninstall-theme <id>`, `set-shell-icon <índice> <icon-id>`, `restore-machine-defaults` | quem chama **nunca** passa caminhos de destino, chaves de registro ou comandos; o ajudante monta tudo a partir de constantes |
| **Cópia segura do staging** (pasta do usuário): abrir com `FILE_SHARE_READ` + `FILE_FLAG_OPEN_REPARSE_POINT`; rejeitar *reparse points* e arquivos com mais de um *hard link*; conferir `GetFinalPathNameByHandle`; calcular o hash **pelo handle aberto** e copiar **desse mesmo handle** para um arquivo `CREATE_NEW` numa pasta criada pelo ajudante com DACL explícita | impede trocar o arquivo entre a verificação e a cópia (TOCTOU) e truques com junctions |
| `.theme` **reconstruído** a partir de chaves permitidas, nunca copiado como veio | um `.theme` pode apontar para caminhos remotos/maliciosos |
| Mitigações: **RedirectionGuard** (`ProcessRedirectionTrustPolicy` em modo *enforce*), `SetDefaultDllDirectories(SYSTEM32)`, não confiar em variáveis de ambiente | não seguir junctions criadas por não-admin; não carregar DLL plantada |
| Faz **só trabalho de máquina** (HKLM, `%WinDir%`); a parte por usuário (aplicar o `.theme`, ícones HKCU) acontece depois, **sem** elevação | com o recurso *Administrator protection* do Windows (lançado em ago/2026, previsto como padrão no 26H2) o processo elevado roda numa conta oculta separada — o HKCU dele **não** é o do usuário |
| Diário de desfazer em `%ProgramData%\RicingManagerWindows\` (ACL só admin) + `restore-machine-defaults` | desfazer não depende de arquivos que o usuário (ou malware) possa alterar |
| Assinado (o UAC mostra editor verificado) | o usuário sabe quem está pedindo admin |

### 4.2 Ícones do sistema inteiro

`HKLM\…\Explorer\Shell Icons` via `set-shell-icon`. Muda ícones em todo lugar (inclusive diálogos de arquivo). Riscos: caminho errado = ícones em branco; atualizações do Windows podem desfazer. Os arquivos de ícone ficam numa pasta protegida (só admin escreve) — nunca apontamos HKLM para arquivo que o usuário possa trocar. Fase 3.

### 4.3 Temas visuais completos (`.msstyles`) — o item mais arriscado

O que é preciso, sendo honesto:
1. **Copiar o tema** para `%WinDir%\Resources\Themes\<nome>\` → nosso ajudante elevado (`install-theme`).
2. **Liberar temas não assinados** → o Windows só aceita `.msstyles` assinados pela Microsoft. Isso exige uma ferramenta de terceiros que **injeta no `winlogon`/`LogonUI`/Explorer**:
   - **Windhawk + mod `uxtheme-hook`** (preferido; o usuário precisa incluir `winlogon.exe` e `logonui.exe` na lista do Windhawk), ou
   - **SecureUxTheme** (LGPL, via IFEO `VerifierDlls`) — **sem manutenção**, com relatos em 2026 de voltar ao tema padrão e de problemas no 25H2.
   - **Nunca**: UltraUXThemePatcher (patcha `uxtheme.dll` no System32).
3. **Aplicar** o `.theme` (sem admin).

**Risco real: loop de login / tela preta.** Por isso:
- Só **experimental**, Fase 3, e **desligado** a menos que o usuário passe pela tela de consentimento com o texto do risco de login.
- Antes de instalar: **ponto de restauração do sistema** (o Windows limita a 1 a cada 24 h) e exibição do procedimento de recuperação (Modo de Segurança → voltar ao tema padrão; WinRE → remover as chaves IFEO, no caso do SecureUxTheme).
- **Segurança do arquivo:** um `.msstyles` é uma DLL de recursos carregada em **todos** os processos, inclusive os elevados — e já houve execução remota de código por tema malicioso (CVE-2023-38146, "ThemeBleed"). Então: `.msstyles` **nunca** vem dentro de um `.rice`; só por importação manual de um arquivo que o usuário baixou do autor, com o hash exibido e aviso.
- **Não redistribuímos temas de terceiros**: os mais populares (ex.: niivu) são "uso pessoal, não redistribuir" — o app só **linka** para a página do autor e importa o arquivo que o usuário baixou. Mesmo temas com licença MIT costumam ser derivados do `aero.msstyles` da Microsoft.
- Temas `.msstyles` quebram com mudanças internas do Windows a cada versão → o app mostra para quais builds o tema foi feito e avisa em caso de divergência.

## 5. Onde isso entra no roadmap

| Item | Fase |
|---|---|
| Integração TranslucentTB (instalar por clique + configurar pela paleta do rice) | **v1.1** |
| Ícones e fontes por usuário (nível 0) | Fase 2 |
| Integração Windhawk (instalar por clique + YAML/CLI) | Fase 2 |
| Componente "Ferramentas avançadas" + `rmw-elevated.exe` | Fase 3 |
| Ícones do sistema inteiro (HKLM) | Fase 3 |
| Temas `.msstyles` completos | Fase 3, **experimental** |

## 6. Ameaças novas e como ficam cobertas (complementa doc 06 §1)

| Ameaça | Controle |
|---|---|
| Malware do usuário usa nosso ajudante para virar admin | ajudante em `%ProgramFiles%`, assinado, UAC por operação, lista fechada de comandos, sem caminhos vindos de fora, cópia TOCTOU-safe, RedirectionGuard |
| Tema malicioso (ThemeBleed e afins) | `.msstyles` nunca vem em `.rice`; só importação manual com aviso; `.theme` reconstruído |
| Ferramenta de terceiros quebra o login/Explorer | consentimento explícito, ponto de restauração, procedimento de recuperação exibido antes, nosso modo seguro desfaz o que é nosso |
| Reputação do nosso app (antivírus/SmartScreen) | nosso binário continua sem injeção; ferramentas que injetam são instaladas pelo winget/Store como pacotes separados, nunca empacotadas |

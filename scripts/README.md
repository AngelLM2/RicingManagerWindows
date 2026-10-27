# Automação de Infraestrutura e Scripts de Desenvolvimento

Este diretório contém a suíte completa de scripts PowerShell para orquestração de compilação, testes em ambiente isolado (Windows Sandbox), análise estática, fuzzing de segurança, empacotamento de releases e geração de SBOM no **Ricing Manager for Windows**.

---

## 1. Visão Geral da Pipeline de Automação

Todos os scripts são construídos para funcionar tanto localmente na máquina do desenvolvedor quanto nos runners do GitHub Actions (CI/CD), garantindo paridade absoluta de execução:

```
+-------------------------------------------------------------------------------+
|                               scripts/env.ps1                                 |
|            Configuração do Ambiente Visual Studio Developer (vsdev.ps1)        |
|                  Tratamento de caches do vcpkg e caminhos seguros              |
+---------------------------------------+---------------------------------------+
                                        |
       +----------------+---------------+---------------+----------------+
       |                |                               |                |
       v                v                               v                v
+--------------+ +--------------+               +---------------+ +---------------+
|  build.ps1   | | sandbox.ps1  |               |   fuzz.ps1    | |  package.ps1  |
|              | |              |               |               | |               |
| - Debug      | | - Windows    |               | - libFuzzer   | | - Release bin |
| - Release    | |   Sandbox    |               | - Sanitizers  | | - Inno Setup  |
| - ASan       | | - Isolamento |               | - Seed Corpus | | - Rices/Plugin|
| - CTest      | | - Testes UI  |               | - Triage      | | - SBOM SPDX   |
+--------------+ +--------------+               +---------------+ +---------------+
```

---

## 2. Catálogo de Scripts

### 2.1 Compilação e Testes (`build.ps1`)
Orquestra o ciclo de vida do CMake e CTest utilizando os presets declarados em `CMakePresets.json`.
- **Presets suportados**:
  - `debug`: Compilação padrão com símbolos e assertions de desenvolvimento.
  - `release`: Otimização máxima `/O2` para benchmarks e empacotamento.
  - `asan`: Instrumentação AddressSanitizer para detecção em runtime de estouros de buffer, use-after-free e memory corruption.
- **Exemplos de uso**:
  ```powershell
  .\scripts\build.ps1                      # Debug build
  .\scripts\build.ps1 -Preset release      # Release build otimizado
  .\scripts\build.ps1 -Preset asan -Test   # ASan build completo com execução de testes
  .\scripts\build.ps1 -Fresh               # Limpa o cache do CMake antes de configurar
  ```

### 2.2 Isolamento de Testes de Sistema (`sandbox.ps1`)
Executa suites de testes destrutivos ou de modificação do sistema (alteração de papel de parede, fontes, temas, cursores e configurações da barra de tarefas) dentro do **Windows Sandbox descartável**, sem tocar no registro ou configurações da máquina host.
- **Funcionamento**:
  - Gera um arquivo `.wsb` dinâmico montando a pasta de build em modo somente leitura (`ReadOnly=true`).
  - Mapeia diretório de saída em `build\sandbox\<preset>` com permissão de escrita para coleta de logs e relatórios JUnit.
  - Monitora o encerramento do processo e destrói o container automaticamente após a execução.
- **Exemplo de uso**:
  ```powershell
  .\scripts\sandbox.ps1 -Preset debug -Suite unit
  .\scripts\sandbox.ps1 -Preset release -Suite system -KeepOpen
  ```

### 2.3 Fuzzing de Robustez com libFuzzer (`fuzz.ps1`)
Compila os alvos de fuzzing (`fuzz/`) instrumentados com LLVM libFuzzer e executa mutações direcionadas com corpora iniciais.
- **Alvos fuzzed**: decodificadores TOML (`rice_toml`), parsers de temas e widgets de plugins.
- **Mecanismo de Corpus**: Corpora persistentes armazenados em `build\fuzz\corpus\<target>`, com dumps automáticos de crash e timeouts gravados em `build\fuzz\crashes\<target>`.
- **Exemplo de uso**:
  ```powershell
  .\scripts\fuzz.ps1                                   # Smoke test rápido no CI (60s por alvo)
  .\scripts\fuzz.ps1 -Target rice_toml -Seconds 3600   # Fuzz intensivo de 1 hora
  ```

### 2.4 Empacotamento de Releases (`package.ps1`)
Gera os artefatos finais distribuíveis da aplicação:
1. Compilação `release` completa com suite de testes e empacotamento dos temas oficiais (`.rice`).
2. Assinatura opcional de binários via Authenticode (`-Sign`).
3. Compilação do instalador per-user com **Inno Setup**.
4. Empacotamento de plugins de exemplo (`.rmwp`).
5. Geração automatizada do manifesto de integridade `SHA256SUMS.txt` e do Software Bill of Materials `sbom.spdx.json`.

### 2.5 Formatação e Qualidade de Código
- `format.ps1`: Executa o `clang-format` recursivamente em todos os fontes C++ do projeto (`apps/`, `libs/`, `plugins/`, `tests/`).
- `tidy.ps1`: Roda o `clang-tidy` aplicando verificações de Core Guidelines, modern-C++ e prevenção de bugs.
- `sign.ps1`: Assina binários e instaladores utilizando certificados de código.
- `translations.ps1`: Sincroniza e valida arquivos de internacionalização (i18n).
- `vsdev.ps1`: Detecta instalações do Visual Studio (Build Tools, Community, Professional, Enterprise) e injeta as variáveis de ambiente do compilador MSVC (`vcvarsall.bat`).

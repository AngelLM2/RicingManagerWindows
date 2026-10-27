# Documentation — Ricing Manager for Windows

> [Português](../README.md) · English

Per-user Windows customization with shareable rices, restoration, and isolated components. This index points to the English guides and to the original Portuguese design history where an English translation is not yet available. Documents 01–13 describe the original plan; their targets and decisions do not necessarily reflect the current implementation.

## Reading paths

| Goal | Read |
|---|---|
| Install and use the application | [Project README](../../README.md#english) → [User guide](../user-guide.md) |
| Understand the project | [Technical overview](architecture.md) → [original architecture](../03-arquitetura.md) (Portuguese) → [architecture decisions](../adr/) (Portuguese) |
| Build and contribute | [Contributing](../../CONTRIBUTING.md) → [engineering and tests](../08-engenharia-build-testes-ci.md) (Portuguese) → [system tests](../../tests/system/README.md) |
| Create plugins | [Plugin author guide](plugins.md) → [API reference](../plugins/api.md) and [security](../plugins/seguranca.md) (Portuguese) |
| Investigate restoration | [Technical overview](architecture.md#application-and-restoration) → [ADR-0026](../adr/0026-o-windows-da-pessoa.md) → [v1.9 plan](../17-plano-v1.9.md) (both Portuguese) |

## Reference

| Document | Contents |
|---|---|
| [Technical overview](architecture.md) | Processes, application flow, data, tests, and diagnostics |
| [User guide](../user-guide.md) | Installation, interface, rices, and features |
| [QA checklist](qa-checklist.md) | Release checks and real-PC verification |
| [Progress journal](../PROGRESSO.md) (Portuguese) | Implementation history and results recorded at each stage |
| [Security](../06-seguranca-memoria-e-estabilidade.md) (Portuguese) | Threats, isolation, and limits; [reporting vulnerabilities](../../SECURITY.md) |
| [Plugin author guide](plugins.md) | Plugin kinds, isolation, commands, and publishing |
| [Architecture decisions](../adr/) (Portuguese) | Decisions and consequences, ordered by number |

## Planning and evolution history

The original documents are available in Portuguese:

| Documents | Subject |
|---|---|
| [01](../01-visao-e-principios.md) · [02](../02-auditoria-codigo-atual.md) · [03](../03-arquitetura.md) | Vision, initial audit, and proposed architecture |
| [04](../04-mvp.md) · [05](../05-roadmap-e-escalabilidade.md) | MVP and original roadmap |
| [06](../06-seguranca-memoria-e-estabilidade.md) · [07](../07-interface-ui.md) · [08](../08-engenharia-build-testes-ci.md) | Security, interface, and engineering |
| [09](../09-pesquisa-tecnica.md) · [10](../10-modo-avancado-e-integracoes.md) · [11](../11-pacote-inicial.md) · [12](../12-plugins.md) | Research, integrations, starter pack, and plugins |
| [13](../13-plano-de-execucao.md) · [14](../14-plano-v1.1.md) · [15](../15-plano-v1.7.md) · [16](../16-plano-v1.8.md) · [17](../17-plano-v1.9.md) | Execution and later version plans |

Results and counts in historical documents record runs at the time; they do not guarantee a current run. Run the relevant tests and inspect their output for the change you are evaluating.

# Documentação — Ricing Manager for Windows

Personalização do Windows por usuário, com rices compartilháveis, restauração e componentes isolados. Esta página é o ponto de entrada; os documentos numerados 01–13 registram o planejamento original e podem conter metas e decisões históricas, não necessariamente o estado atual.

## Trilhas de leitura

| Objetivo | Leia |
|---|---|
| Usar o aplicativo | [README](../README.md) → [guia de uso](user-guide.md) |
| Entender o projeto | [Visão técnica](arquitetura-atual.md) → [arquitetura original](03-arquitetura.md) → [decisões](adr/) |
| Compilar e contribuir | [Contributing](../CONTRIBUTING.md) → [engenharia e testes](08-engenharia-build-testes-ci.md) → [testes de sistema](../tests/system/README.md) |
| Criar plugins | [Guia de plugins](plugins/README.md) → [API](plugins/api.md) → [segurança](plugins/seguranca.md) |
| Investigar restauração | [Visão técnica](arquitetura-atual.md#aplicação-e-restauração) → [ADR-0026](adr/0026-o-windows-da-pessoa.md) → [plano v1.9](17-plano-v1.9.md) |

## Referência

| Documento | Conteúdo |
|---|---|
| [Visão técnica](arquitetura-atual.md) | processos, fluxo de aplicação, dados, testes e diagnóstico |
| [Guia de uso](user-guide.md) | instalação, interface, rices e recursos |
| [Checklist de QA](qa-checklist.md) | verificações manuais |
| [Diário de progresso](PROGRESSO.md) | histórico de implementação e resultados registrados por etapa |
| [Segurança](06-seguranca-memoria-e-estabilidade.md) | ameaças, isolamento e limites; [reporte de vulnerabilidades](../SECURITY.md) |
| [Plugin API](plugins/README.md) | manifesto, API, widgets, tweaks e permissões |
| [ADRs](adr/) | decisões e consequências, ordenadas por número |

## Histórico de planejamento e evolução

| Documento | Assunto |
|---|---|
| [01](01-visao-e-principios.md) · [02](02-auditoria-codigo-atual.md) · [03](03-arquitetura.md) | visão, auditoria inicial e arquitetura proposta |
| [04](04-mvp.md) · [05](05-roadmap-e-escalabilidade.md) | MVP e roadmap original |
| [06](06-seguranca-memoria-e-estabilidade.md) · [07](07-interface-ui.md) · [08](08-engenharia-build-testes-ci.md) | segurança, interface e engenharia |
| [09](09-pesquisa-tecnica.md) · [10](10-modo-avancado-e-integracoes.md) · [11](11-pacote-inicial.md) · [12](12-plugins.md) | pesquisa, integrações, pacote e plugins |
| [13](13-plano-de-execucao.md) · [14](14-plano-v1.1.md) · [15](15-plano-v1.7.md) · [16](16-plano-v1.8.md) · [17](17-plano-v1.9.md) | execução e planos de versões posteriores |

Resultados e contagens em documentos históricos são registros da época, não garantias de uma execução atual. Para conferir o estado de uma alteração, execute os testes indicados e consulte os resultados produzidos na sua máquina.

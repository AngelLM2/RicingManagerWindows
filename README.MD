# Ricing Manager for Windows

Um projeto em C++ para ajudar na personalização visual do Windows, com foco em gerenciamento de papéis de parede, temas e componentes modulares para facilitar futuras extensões.

## O que é este projeto?

O Ricing Manager for Windows é uma aplicação experimental voltada para automação de personalização de ambiente desktop. A ideia é fornecer uma base organizada para:

- alterar papéis de parede;
- gerenciar configurações de tema;
- adicionar plugins no futuro;
- manter uma arquitetura modular e escalável.

## Funcionalidades

Atualmente, o projeto já possui uma estrutura inicial com:

- gerenciamento de wallpaper;
- camada de logging para mensagens de erro e informação;
- configuração centralizada;
- arquitetura baseada em managers e módulos separados.

## Estrutura do projeto

```text
src/
├── App.cpp / App.hpp
├── main.cpp
├── config/
├── GUI/
├── logging/
├── managers/
│   ├── explorer/
│   ├── plugins/
│   ├── registry/
│   ├── theme/
│   └── wallpaper/
├── Settings/
└── utils/
```

## Requisitos

- Windows 10/11
- CMake 3.10 ou superior
- Compilador com suporte a C++20 (recomendado: MSVC/Visual Studio)
- Git para controlar versões

## Como compilar e executar

### 1. Clone o repositório

```bash
git clone https://github.com/seu-usuario/RicingManagerWindows.git
cd RicingManagerWindows
```

### 2. Gere os arquivos de build

```bash
cmake -S . -B build
```

### 3. Compile o projeto

```bash
cmake --build build
```

### 4. Execute a aplicação

```bash
./build/Debug/RicingManagerWindows.exe
```

> Em alguns ambientes Windows, o executável pode estar em outra pasta de configuração do build, como `build/Release/`.

## Status do projeto

Este projeto está em fase inicial de desenvolvimento. A estrutura já foi organizada para crescimento, mas ainda há espaço para melhorias em:

- integração completa de temas;
- suporte a plugins;
- interface gráfica;
- configuração persistente do usuário.

## Contribuição

Contribuições são bem-vindas. Se quiser colaborar:

1. faça um fork do projeto;
2. crie uma branch para sua feature;
3. envie um pull request com descrição clara das mudanças.

## Observação

Este projeto foi criado com o objetivo de servir como base para um gerenciador de personalização mais completo no ambiente Windows.

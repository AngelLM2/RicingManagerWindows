#pragma once

#include <filesystem>
#include <string_view>

// Forward declaration.
// O sqlite3.h completo só será incluído no Database.cpp.
struct sqlite3;

class Database
{
public:
    // Recebe o caminho do arquivo .db.
    // Não abre o banco automaticamente.
    explicit Database(std::filesystem::path databasePath);

    // Fecha a conexão automaticamente.
    ~Database();

    // Uma conexão SQLite não deve ser copiada.
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // A conexão pode ter sua propriedade transferida.
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // Abre ou cria o arquivo do banco.
    [[nodiscard]] bool open();

    // Fecha a conexão.
    void close() noexcept;

    // Executa SQL que não precisa retornar registros.
    // Exemplos: CREATE TABLE, INSERT fixo, UPDATE, PRAGMA.
    [[nodiscard]] bool execute(std::string_view sql);

    // Controle de transações.
    [[nodiscard]] bool beginTransaction();
    [[nodiscard]] bool commit();
    [[nodiscard]] bool rollback();

    // Retorna true se existe uma conexão aberta.
    [[nodiscard]] bool isOpen() const noexcept;

    // Retorna o ponteiro nativo do SQLite.
    // Não transfere a propriedade da conexão.
    [[nodiscard]] sqlite3* handle() const noexcept;

    // Retorna o caminho do arquivo do banco.
    [[nodiscard]]
    const std::filesystem::path& path() const noexcept;

private:
    // Configura a conexão após a abertura.
    [[nodiscard]] bool configure();

    std::filesystem::path m_databasePath;
    sqlite3* m_database = nullptr;
};
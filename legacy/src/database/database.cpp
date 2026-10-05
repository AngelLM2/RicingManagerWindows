#include "Database.hpp"

#include <sqlite3.h>

#include <filesystem>
#include <format>
#include <string>
#include <utility>

#include "logging/Logger.hpp"

namespace
{
    /*
     * No Windows, std::filesystem::path trabalha com o formato
     * nativo da plataforma, normalmente UTF-16.
     *
     * sqlite3_open_v2 recebe const char* em UTF-8.
     *
     * Esta função realiza a conversão:
     *
     * filesystem::path
     *      ↓
     * std::u8string
     *      ↓
     * std::string contendo bytes UTF-8
     */
    std::string pathToUtf8(
        const std::filesystem::path& path
    )
    {
        const std::u8string utf8Path = path.u8string();

        return std::string(
            reinterpret_cast<const char*>(utf8Path.data()),
            utf8Path.size()
        );
    }

    /*
     * sqlite3_errmsg16 retorna uma mensagem UTF-16.
     *
     * Isso funciona diretamente com std::wstring no Windows,
     * pois wchar_t possui 16 bits nessa plataforma.
     */
    std::wstring sqliteErrorMessage(sqlite3* database)
    {
        if (database == nullptr)
        {
            return L"Unknown SQLite error";
        }

        const void* rawMessage = sqlite3_errmsg16(database);

        if (rawMessage == nullptr)
        {
            return L"Unknown SQLite error";
        }

        return static_cast<const wchar_t*>(rawMessage);
    }
}

Database::Database(std::filesystem::path databasePath)
    : m_databasePath(std::move(databasePath))
{
}

Database::~Database()
{
    close();
}

Database::Database(Database&& other) noexcept
    : m_databasePath(std::move(other.m_databasePath)),
      m_database(other.m_database)
{
    /*
     * A conexão agora pertence ao novo objeto.
     * O objeto antigo não pode mais fechá-la.
     */
    other.m_database = nullptr;
}

Database& Database::operator=(Database&& other) noexcept
{
    if (this == &other)
    {
        return *this;
    }

    /*
     * Fecha uma eventual conexão que este objeto
     * já possuía antes de receber a nova.
     */
    close();

    m_databasePath = std::move(other.m_databasePath);
    m_database = other.m_database;

    /*
     * Remove a propriedade da conexão do objeto antigo.
     */
    other.m_database = nullptr;

    return *this;
}

bool Database::open()
{
    Logger& logger = Logger::getInstance();

    /*
     * Abrir novamente uma conexão já aberta não é erro.
     */
    if (isOpen())
    {
        return true;
    }

    if (m_databasePath.empty())
    {
        logger.error(L"Database path cannot be empty.");
        return false;
    }

    /*
     * O diretório pode ainda não existir.
     *
     * Exemplo:
     * C:\Users\Angel\AppData\Local\RicingManager\data.db
     */
    const std::filesystem::path parentDirectory =
        m_databasePath.parent_path();

    if (!parentDirectory.empty())
    {
        std::error_code error;

        std::filesystem::create_directories(
            parentDirectory,
            error
        );

        if (error)
        {
            logger.error(std::format(
                L"Failed to create database directory '{}'. "
                L"Filesystem error code: {}",
                parentDirectory.wstring(),
                error.value()
            ));

            return false;
        }
    }

    /*
     * Converte o caminho nativo do Windows para UTF-8,
     * como exigido por sqlite3_open_v2.
     */
    const std::string utf8DatabasePath =
        pathToUtf8(m_databasePath);

    const int openFlags =
        SQLITE_OPEN_READWRITE |
        SQLITE_OPEN_CREATE |
        SQLITE_OPEN_FULLMUTEX;

    const int result = sqlite3_open_v2(
        utf8DatabasePath.c_str(),
        &m_database,
        openFlags,
        nullptr
    );

    if (result != SQLITE_OK)
    {
        const std::wstring message =
            sqliteErrorMessage(m_database);

        logger.error(std::format(
            L"Failed to open database '{}'. "
            L"SQLite code: {}. Error: {}",
            m_databasePath.wstring(),
            result,
            message
        ));

        /*
         * Mesmo quando sqlite3_open_v2 falha, o SQLite
         * pode criar uma estrutura sqlite3 que precisa
         * ser fechada.
         */
        close();

        return false;
    }

    if (!configure())
    {
        logger.error(std::format(
            L"Failed to configure database '{}'.",
            m_databasePath.wstring()
        ));

        close();

        return false;
    }

    logger.info(std::format(
        L"Database opened successfully: {}",
        m_databasePath.wstring()
    ));

    return true;
}

void Database::close() noexcept
{
    if (m_database == nullptr)
    {
        return;
    }

    /*
     * close_v2 é mais tolerante do que sqlite3_close.
     *
     * Se ainda existirem prepared statements,
     * o fechamento real será adiado até eles serem
     * finalizados.
     */
    const int result = sqlite3_close_v2(m_database);

    if (result == SQLITE_OK)
    {
        m_database = nullptr;
        return;
    }

    /*
     * close() é noexcept porque também é chamado pelo
     * destrutor. Nenhuma exceção pode escapar daqui.
     */
    try
    {
        Logger::getInstance().error(
            L"Failed to close SQLite database."
        );
    }
    catch (...)
    {
        // Um destrutor nunca deve propagar exceções.
    }
}

bool Database::execute(std::string_view sql)
{
    Logger& logger = Logger::getInstance();

    if (!isOpen())
    {
        logger.error(
            L"Cannot execute SQL because the database is not open."
        );

        return false;
    }

    if (sql.empty())
    {
        logger.error(L"Cannot execute empty SQL.");
        return false;
    }

    /*
     * string_view não garante que o texto termine com '\0'.
     * sqlite3_exec exige uma string terminada em '\0'.
     *
     * Portanto, criamos uma std::string.
     */
    const std::string sqlString(sql);

    const int result = sqlite3_exec(
        m_database,
        sqlString.c_str(),
        nullptr,
        nullptr,
        nullptr
    );

    if (result != SQLITE_OK)
    {
        logger.error(std::format(
            L"SQLite execution failed. "
            L"Code: {}. Error: {}",
            result,
            sqliteErrorMessage(m_database)
        ));

        return false;
    }

    return true;
}

bool Database::beginTransaction()
{
    /*
     * IMMEDIATE adquire o bloqueio de escrita no início
     * da transação, em vez de esperar o primeiro INSERT,
     * UPDATE ou DELETE.
     */
    return execute("BEGIN IMMEDIATE TRANSACTION;");
}

bool Database::commit()
{
    return execute("COMMIT;");
}

bool Database::rollback()
{
    return execute("ROLLBACK;");
}

bool Database::isOpen() const noexcept
{
    return m_database != nullptr;
}

sqlite3* Database::handle() const noexcept
{
    /*
     * Apenas empresta o ponteiro.
     *
     * Quem chamar handle() não deve executar:
     *
     * sqlite3_close(handle());
     */
    return m_database;
}

const std::filesystem::path&
Database::path() const noexcept
{
    return m_databasePath;
}

bool Database::configure()
{
    if (!isOpen())
    {
        return false;
    }

    /*
     * Faz o SQLite fornecer códigos de erro mais
     * específicos quando possível.
     */
    const int extendedResult =
        sqlite3_extended_result_codes(m_database, 1);

    if (extendedResult != SQLITE_OK)
    {
        Logger::getInstance().error(std::format(
            L"Failed to enable extended SQLite result codes: {}",
            sqliteErrorMessage(m_database)
        ));

        return false;
    }

    /*
     * Quando o banco estiver temporariamente ocupado,
     * espera até 5 segundos antes de retornar SQLITE_BUSY.
     */
    const int timeoutResult =
        sqlite3_busy_timeout(m_database, 5000);

    if (timeoutResult != SQLITE_OK)
    {
        Logger::getInstance().error(std::format(
            L"Failed to configure SQLite busy timeout: {}",
            sqliteErrorMessage(m_database)
        ));

        return false;
    }

    /*
     * No SQLite, foreign keys precisam ser ativadas
     * individualmente para cada conexão.
     */
    if (!execute("PRAGMA foreign_keys = ON;"))
    {
        return false;
    }

    /*
     * WAL permite melhor convivência entre leitura e escrita
     * e costuma ser adequado para aplicativos desktop.
     */
    if (!execute("PRAGMA journal_mode = WAL;"))
    {
        return false;
    }

    /*
     * NORMAL oferece um bom equilíbrio entre desempenho
     * e segurança quando WAL está ativado.
     */
    if (!execute("PRAGMA synchronous = NORMAL;"))
    {
        return false;
    }

    return true;
}
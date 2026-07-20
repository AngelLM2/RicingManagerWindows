#pragma once

#include <mutex>
#include <string>

enum class LogLevel {
    LEVEL_INFO,
    LEVEL_WARNING,
    LEVEL_ERROR
};

class Logger {
public:
    static Logger& getInstance(){
        static Logger instance; 

        return instance;
    };

    void info(const std::wstring& message);
    void warning(const std::wstring& message);
    void error(const std::wstring& message);

private:
    Logger() = default;
    void log(LogLevel level, const std::wstring& message);

    std::mutex m_mutex;
};
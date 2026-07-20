#include <string>
#include <mutex>
#include <chrono>
#include <iostream>

#include "Logger.hpp"


// .info()
void Logger::info(const std::wstring& message){
    log(LogLevel::LEVEL_INFO, message);
}

// .warning()
void Logger::warning(const std::wstring& message){
    log(LogLevel::LEVEL_WARNING, message);
}

// .error()
void Logger::error(const std::wstring& message){
    log(LogLevel::LEVEL_ERROR, message);
}

void Logger::log(LogLevel level, const std::wstring& message) {
    std::lock_guard<std::mutex> lock(m_mutex); //bloqueia e desbloqueia

    std::wstring levelStr;
    switch (level){
        case LogLevel::LEVEL_INFO: levelStr = L"[INFO]"; break;
        case LogLevel::LEVEL_WARNING: levelStr = L"[WARN]"; break;
        case LogLevel::LEVEL_ERROR: levelStr = L"[ERR]"; break;
    }

    std::wcout << std::format(L"{} {}\n", levelStr, message);
}


#define OEMRESOURCE

#include "CursorManager.hpp" 
#include "CursorHandler.hpp"   
#include "logging/logger.hpp"
#include "utils/WinError.hpp"
#include "CursorTypeConverter.hpp"


#include <Windows.h>
#include <format>



bool CursorManager::applyCursor(CursorType type, const std::wstring& path)
{
    Logger& logger = Logger::getInstance();

    HANDLE image = LoadCursorFromFileW(path.c_str());

    if (image == nullptr)
    {
        DWORD error = GetLastError();
        logger.error(std::format(L"Error: {}", WinError::message(error)));
        return false;
    }

    HCURSOR cursor = LoadCursorFromFileW(path.c_str());

    CursorHandler cursorHandler(cursor);

    BOOL applied = SetSystemCursor(cursorHandler.release(), CursorTypeConverter::toOCR(type) );

    if (!applied)
    {
        DWORD error = GetLastError();
        logger.error(std::format(L"Error: {}", WinError::message(error)));
        return false;
    }

    return true;
}

bool CursorManager::restoreDefaultCursors()
{
    BOOL restored = SystemParametersInfoW(
        SPI_SETCURSORS,
        0,
        nullptr,
        0
    );

    return restored != FALSE;
}

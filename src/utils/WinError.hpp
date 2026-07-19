#pragma once

#include <string>
#include <windows.h>

class WinError
{
    public:
        static std::wstring message(DWORD errorCode);

};
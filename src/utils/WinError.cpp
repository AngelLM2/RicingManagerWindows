#include "WinError.hpp"

std::wstring WinError::message(DWORD errorCode)
{
    LPWSTR buffer = nullptr; 

    /* FormatMessageW is a Windows API function that formats a message string from
    a specified source, using a message identifier and optional arguments.
    It supports Unicode and is typically used to retrieve error messages or
    other localized strings based on the provided parameters.*/

    DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM,
        nullptr,
        errorCode,
        0,
        (LPWSTR)&buffer, // passando endereço da variavel que guarda ponteiro do buffer
        0,
        nullptr
    );

    if (length == 0)
    {
       return L"Unknown Windows error"; 
    }

    std::wstring message(buffer); // copia a mensagem para outro alocamento

    LocalFree(buffer); // libera a memoria
    buffer = nullptr;

    return message;

};
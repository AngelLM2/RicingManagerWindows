#pragma once

#include "CursorType.hpp"

#include <string>


class CursorManager
{
    public:
            bool applyCursor(CursorType type ,const std::wstring& path);

            bool restoreDefaultCursors();
};
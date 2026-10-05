#include "CursorHandler.hpp"


CursorHandler::CursorHandler(HCURSOR handle): m_handle(handle)
{
    
}


CursorHandler::~CursorHandler()
{
    if(m_handle)
    {
        DestroyCursor(m_handle);
    }
}

HCURSOR CursorHandler::get() const
{
    return m_handle;
}

HCURSOR CursorHandler::release()
{
    HCURSOR handle = m_handle;
    m_handle = nullptr;
    return handle;
}
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
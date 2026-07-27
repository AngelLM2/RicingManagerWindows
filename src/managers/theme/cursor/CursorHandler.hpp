#pragma once

#include <windows.h>


class CursorHandler
{
    private:
        // gerenciar vida de um HCURSOR
        // quem cria destroi RAII
        HCURSOR m_handle; 


    public:
        // recebe um HCURSOR criado pelo windows
        // Ex:
        // HCURSOR cursor = LoadCursorFromFileW(...);
        // CursorHandle handle(cursor);
        explicit CursorHandler(HCURSOR handle);
        
        // quando destruido libera o cursor
        ~CursorHandler();

        //Retorna o cursor
        //Nao remove posse

        HCURSOR get() const;

        // entrega a posse a outra entidade
        // cursor handler perde responsa

        HCURSOR release();

};
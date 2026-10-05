#pragma once
#include <windows.h>
#include <string>
#include <vector>   

// Estrutura para armazenar informações do monitor
struct MonitorInfo {
    UINT index;
    std::wstring devicePath;
    std::wstring friendlyName;
};

class MonitorID
{
    public:

        //inicializa a lista de monitores(chama get monitor device)
        HRESULT Initialize();

        //retorna quantidade de monitores detectador
        UINT GetCount() const;

        //retorna info de um monitor pelo indice
        const MonitorInfo* GetMonitorInfo(UINT index) const;

        //lista monitores no console
        void ListMonitors() const;

        //select monitor
        bool SelectMonitor(UINT index);

        UINT GetSelectedIndex() const;

        std::wstring GetDevicePath(UINT index) const;

        int ChooseMonitor(int index);

    private:
        std::vector<MonitorInfo> m_monitors;
        UINT m_selectedIndex;

        //função auxiliar para obter nome amigavel apartir do devicepath
        std::wstring GetMonitorFriendlyName(const std::wstring& devicePath);
};
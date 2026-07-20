#include "MonitorID.hpp"
#include <shobjidl_core.h> // Necessário para IDesktopWallpaper
#include <iostream>
#include "../logging/Logger.hpp"



#pragma comment(lib, "ole32.lib")

MonitorID::MonitorID() : m_selectedIndex(UINT_MAX)
{
    // Inicializa COM se necessário 
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
}

MonitorID::~MonitorID()
{
    CoUninitialize();
}

HRESULT MonitorID::Initialize()
{
    m_monitors.clear();
    m_selectedIndex = UINT_MAX;

    IDesktopWallpaper* pDesktopWallpaper = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_DesktopWallpaper,
        nullptr,
        CLSCTX_ALL,
        IID_IDesktopWallpaper,
        reinterpret_cast<void**>(&pDesktopWallpaper)
    );

    if (FAILED(hr)) {
        return hr;
    }

    UINT count = 0;
    hr = pDesktopWallpaper->GetMonitorDevicePathCount(&count);
    
    if (SUCCEEDED(hr)) {
        for (UINT i = 0; i < count; ++i) {
            LPWSTR devicePath = nullptr;
            hr = pDesktopWallpaper->GetMonitorDevicePathAt(i, &devicePath);

            if (SUCCEEDED(hr) && devicePath) {
                MonitorInfo info;
                info.index = i;
                info.devicePath = std::wstring(devicePath);
                info.friendlyName = GetMonitorFriendlyName(info.devicePath);
                
                m_monitors.push_back(info);
                
                // Libera a memória alocada pela API COM
                CoTaskMemFree(devicePath);
            }
        }
    }

    pDesktopWallpaper->Release();
    return hr;
}

UINT MonitorID::GetCount() const
{
    return static_cast<UINT>(m_monitors.size());
}

const MonitorInfo* MonitorID::GetMonitorInfo(UINT index) const
{
    if (index >= m_monitors.size()) {
        return nullptr;
    }
    return &m_monitors[index];
}

bool MonitorID::SelectMonitor(UINT index)
{
    if (index >= m_monitors.size()) {
        return false;
    }
    m_selectedIndex = index;
    return true;
}

UINT MonitorID::GetSelectedIndex() const
{
    return m_selectedIndex;
}

void MonitorID::ListMonitors() const
{
    std::wcout << L"--- Monitores Detectados ---" << std::endl;
    for (const auto& mon : m_monitors) {
        std::wcout << L"Índice: " << mon.index 
                   << L" | Path: " << mon.devicePath 
                   << L" | Nome: " << mon.friendlyName << std::endl;
    }
    std::wcout << L"--------------------------" << std::endl;
}

// WALLPAPER
std::wstring MonitorID::GetDevicePath(UINT index) const {
    const MonitorInfo* info = GetMonitorInfo(index);
    if (info) {
        return info->devicePath; // Retorna uma cópia da string
    }
    return L""; // Retorna vazio se inválido
}

// Opcional: Se quiser que a própria classe MonitorID resolva o wallpaper
std::wstring MonitorID::GetWallpaperPath(UINT index) {
    std::wstring path = L"";
    
    // Inicialização COM local (necessária se esta função for chamada sozinha)
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    
    IDesktopWallpaper* pWallpaper = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_INPROC_SERVER, IID_IDesktopWallpaper, (void**)&pWallpaper))) {
        std::wstring devPath = GetDevicePath(index);
        if (!devPath.empty()) {
            LPWSTR wpPath = nullptr;
            if (SUCCEEDED(pWallpaper->GetWallpaper(devPath.c_str(), &wpPath))) {
                path = std::wstring(wpPath);
                CoTaskMemFree(wpPath);
            }
        }
        pWallpaper->Release();
    }
    
    CoUninitialize();
    return path;
}   

// WALLPAPER



// Implementação da lógica de duas etapas com EnumDisplayDevices
std::wstring MonitorID::GetMonitorFriendlyName(const std::wstring& devicePath)
{
    DISPLAY_DEVICE adapterInfo;
    ZeroMemory(&adapterInfo, sizeof(adapterInfo));
    adapterInfo.cb = sizeof(adapterInfo);

    // Etapa 1: Encontrar o adaptador correspondente ao devicePath (\\.\DISPLAY1)
    DWORD deviceIndex = 0;
    bool adapterFound = false;

    // Itera sobre todos os adaptadores de vídeo
    while (EnumDisplayDevices(nullptr, deviceIndex, &adapterInfo, 0)) {
        if (devicePath == (adapterInfo.DeviceName)) {
            adapterFound = true;
            break;
        }
        ZeroMemory(&adapterInfo, sizeof(adapterInfo));
        adapterInfo.cb = sizeof(adapterInfo);
        deviceIndex++;
    }

    if (!adapterFound) {
        return L"Desconhecido";
    }

    // Etapa 2: Obter o monitor conectado a este adaptador
    DISPLAY_DEVICE monitorInfo;
    ZeroMemory(&monitorInfo, sizeof(monitorInfo));
    monitorInfo.cb = sizeof(monitorInfo);

    // Chama EnumDisplayDevices passando o NOME DO ADAPTADOR como primeiro parâmetro
    // e 0 como índice para pegar o primeiro monitor conectado a ele
    if (EnumDisplayDevices(adapterInfo.DeviceName, 0, &monitorInfo, EDD_GET_DEVICE_INTERFACE_NAME)) {
        // O campo DeviceString contém o nome amigável (ex: "Dell U2417")
        return std::wstring(monitorInfo.DeviceString);
    }

    return L"Generic PnP Monitor";
}   

int MonitorID::ChooseMonitor(int index)
{
    Logger& logger = Logger::getInstance();

    std::cout << "\nChoose the monitor (Enter for 0): ";
    
    std::string input;
    std::getline(std::cin, input);
    
    if (!input.empty())
    {
        try 
        {
            index = std::stoi(input);
        }
        catch (const std::exception&) 
        {
            logger.error(L"Invalid input format. Using default monitor 0.");
            index = 0;
        }
    }
    return index;
}

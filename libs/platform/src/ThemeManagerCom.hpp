#pragma once

// Undocumented theme COM interfaces (layout from Auto Dark Mode / namazso, MIT; verified in spike
// S4, ADR-0005). Internal to rmw::platform.

#include <windows.h>

#include <unknwn.h>

namespace rmw::platform::detail
{

// COM interfaces are released through IUnknown, never deleted: no virtual destructors.
// NOLINTBEGIN(cppcoreguidelines-virtual-class-destructor)

MIDL_INTERFACE("26e4185f-0528-475f-acaf-abe89ba6017d")
ITheme : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(BSTR * name) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DisplayName(BSTR name) = 0;
};

MIDL_INTERFACE("c1e8c83e-845d-4d95-81db-e283fdffc000")
IThemeManager2 : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Init(int flags) = 0;
    virtual HRESULT STDMETHODCALLTYPE InitAsync(HWND hwnd, int unk) = 0;
    virtual HRESULT STDMETHODCALLTYPE Refresh() = 0;
    virtual HRESULT STDMETHODCALLTYPE RefreshAsync(HWND hwnd, int unk) = 0;
    virtual HRESULT STDMETHODCALLTYPE RefreshComplete() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetThemeCount(int* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetTheme(int index, ITheme** theme) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsThemeDisabled(int index, int* disabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentTheme(int* index) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCurrentTheme(HWND parent, int index, int applyNow, ULONG applyFlags,
                                                      ULONG packFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCustomTheme(int* index) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultTheme(int* index) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateThemePack(HWND hwnd, BSTR unk1, ULONG packFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE CloneAndSetCurrentTheme(HWND hwnd, BSTR unk1, BSTR * unk2) = 0;
    virtual HRESULT STDMETHODCALLTYPE InstallThemePack(HWND hwnd, BSTR unk1, int unk2, ULONG packFlags,
                                                       BSTR* unk3, ITheme** unk4) = 0;
    virtual HRESULT STDMETHODCALLTYPE DeleteTheme(BSTR unk1) = 0;
    virtual HRESULT STDMETHODCALLTYPE OpenTheme(HWND hwnd, BSTR path, ULONG packFlags) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddAndSelectTheme(HWND hwnd, BSTR path, ULONG applyFlags,
                                                        ULONG packFlags) = 0;
};

inline constexpr CLSID kClsidThemeManager2 = {
    0x9324da94, 0x50ec, 0x4a14, {0xa7, 0x70, 0xe9, 0x0c, 0xa0, 0x3e, 0x7c, 0x8f}};

// Legacy IThemeManager (themeui.dll): applies a .theme by path, ignoring apply flags.
MIDL_INTERFACE("D23CC733-5522-406D-8DFB-B3CF5EF52A71")
ILegacyTheme : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(BSTR * name) = 0;
};

MIDL_INTERFACE("0646EBBE-C1B7-4045-8FD0-FFD65D3FC792")
ILegacyThemeManager : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE get_CurrentTheme(ILegacyTheme * *theme) = 0;
    virtual HRESULT STDMETHODCALLTYPE ApplyTheme(BSTR path) = 0;
};

inline constexpr CLSID kClsidLegacyThemeManager = {
    0xC04B329E, 0x5823, 0x4415, {0x9C, 0x93, 0xBA, 0x44, 0x68, 0x89, 0x47, 0xB0}};

// NOLINTEND(cppcoreguidelines-virtual-class-destructor)

// Parts of a theme that SetCurrentTheme must leave alone: we only want mode and accent.
inline constexpr ULONG kIgnoreBackground = 1u << 0;
inline constexpr ULONG kIgnoreCursor = 1u << 1;
inline constexpr ULONG kIgnoreDesktopIcons = 1u << 2;
inline constexpr ULONG kIgnoreSound = 1u << 4;
inline constexpr ULONG kIgnoreScreensaver = 1u << 5;
inline constexpr ULONG kNoHourglass = 1u << 8;
inline constexpr ULONG kApplyOnlyLook = kIgnoreBackground | kIgnoreCursor | kIgnoreDesktopIcons |
                                        kIgnoreSound | kIgnoreScreensaver | kNoHourglass;

} // namespace rmw::platform::detail

#pragma once

#include <string_view>

// Every text the core shows (tray menu and notifications), in one place: pt-BR when the user's
// Windows speaks Portuguese, English otherwise. The settings app brings gettext in M7 (docs/07);
// until then this table is the whole translation.
namespace rmw::core
{

struct UiStrings
{
    std::wstring_view tooltip;
    std::wstring_view tooltipSafeMode;
    std::wstring_view toggleAppearance;
    std::wstring_view restoreOriginal;
    std::wstring_view reloadSettings;
    std::wstring_view exit;
    std::wstring_view safeModeItem;

    std::wstring_view safeModeTitle;
    std::wstring_view safeModeText; // {0} = why
    std::wstring_view safeModeCrashes;
    std::wstring_view safeModeRequested;

    std::wstring_view driftTitle;
    std::wstring_view driftText; // {0} = what left the rice

    std::wstring_view actionFailedTitle;
    std::wstring_view actionFailedText; // {0} = action, {1} = error
    std::wstring_view settingsErrorTitle;
    std::wstring_view settingsErrorText; // {0} = first problem
    std::wstring_view hotkeyTakenText;   // {0} = key chord

    std::wstring_view barOn;
    std::wstring_view barOff;
    std::wstring_view barOffTitle;
    std::wstring_view barOffText;
    std::wstring_view barDisabledText;

    std::wstring_view appearance;
    std::wstring_view cursors;
    std::wstring_view wallpaper;
    std::wstring_view taskbar;
};

inline constexpr UiStrings kEnglish{
    .tooltip = L"Ricing Manager",
    .tooltipSafeMode = L"Ricing Manager (safe mode)",
    .toggleAppearance = L"Switch light/dark mode",
    .restoreOriginal = L"Restore original Windows",
    .reloadSettings = L"Reload settings",
    .exit = L"Exit",
    .safeModeItem = L"Safe mode: nothing runs automatically",
    .safeModeTitle = L"Ricing Manager started in safe mode",
    .safeModeText =
        L"{0} Nothing is applied automatically. You can restore the original Windows from this menu.",
    .safeModeCrashes = L"It stopped unexpectedly the last 3 times it started.",
    .safeModeRequested = L"Safe mode was requested.",
    .driftTitle = L"Changed outside Ricing Manager",
    .driftText = L"{0} no longer follows the rice: your change stays.",
    .actionFailedTitle = L"Ricing Manager",
    .actionFailedText = L"{0} failed: {1}",
    .settingsErrorTitle = L"settings.toml has a problem",
    .settingsErrorText = L"{0}",
    .hotkeyTakenText = L"The shortcut {0} is already used by another program.",
    .barOn = L"Turn the bar on",
    .barOff = L"Turn the bar off",
    .barOffTitle = L"The bar is off",
    .barOffText = L"Windows' taskbar is back. Turn the bar on again from this menu.",
    .barDisabledText = L"The bar stopped several times and was turned off; Windows' taskbar is back.",
    .appearance = L"Appearance",
    .cursors = L"Cursors",
    .wallpaper = L"Wallpaper",
    .taskbar = L"Taskbar",
};

inline constexpr UiStrings kPortuguese{
    .tooltip = L"Ricing Manager",
    .tooltipSafeMode = L"Ricing Manager (modo seguro)",
    .toggleAppearance = L"Alternar modo claro/escuro",
    .restoreOriginal = L"Restaurar Windows original",
    .reloadSettings = L"Recarregar configurações",
    .exit = L"Sair",
    .safeModeItem = L"Modo seguro: nada roda automaticamente",
    .safeModeTitle = L"Ricing Manager iniciou no modo seguro",
    .safeModeText =
        L"{0} Nada é aplicado automaticamente. Você pode restaurar o Windows original por este menu.",
    .safeModeCrashes = L"Ele parou inesperadamente nas últimas 3 vezes em que iniciou.",
    .safeModeRequested = L"O modo seguro foi pedido.",
    .driftTitle = L"Alterado fora do Ricing Manager",
    .driftText = L"{0} deixou de seguir o rice: a sua mudança fica.",
    .actionFailedTitle = L"Ricing Manager",
    .actionFailedText = L"{0} falhou: {1}",
    .settingsErrorTitle = L"O settings.toml tem um problema",
    .settingsErrorText = L"{0}",
    .hotkeyTakenText = L"O atalho {0} já é usado por outro programa.",
    .barOn = L"Ligar a barra",
    .barOff = L"Desligar a barra",
    .barOffTitle = L"A barra foi desligada",
    .barOffText = L"A barra do Windows voltou. Ligue a barra de novo por este menu.",
    .barDisabledText = L"A barra parou várias vezes e foi desligada; a barra do Windows voltou.",
    .appearance = L"A aparência",
    .cursors = L"Os cursores",
    .wallpaper = L"O wallpaper",
    .taskbar = L"A barra de tarefas",
};

// Picked once from the user's Windows display language.
[[nodiscard]] const UiStrings& uiStrings() noexcept;

} // namespace rmw::core

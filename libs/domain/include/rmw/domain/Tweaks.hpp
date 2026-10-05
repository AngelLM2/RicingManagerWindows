#pragma once

#include "rmw/base/Error.hpp"

#include <span>
#include <string>
#include <string_view>

// What a tweak plugin (P1, docs/12 §3) may write: HKEY_CURRENT_USER values under an allow-list of
// keys, never under a fixed block-list (autostart, COM, policies, environment, logon...), never a
// value one of our modules owns, never data that looks like a program to run or a network path
// (docs/06 §6). Pure and fuzzable; the manifest parser and the tweaks module both call it.
namespace rmw::domain
{

// Accepts "HKCU\<key>", "HKEY_CURRENT_USER\<key>" or "<key>" (any case, '\' or '/') and returns the
// subkey as written in the allow-list's canonical form ("Software\Microsoft\...\Advanced" plus the
// rest as given). Rejects other hives, empty or odd components, and anything outside the
// allow-list or inside the block-list.
[[nodiscard]] Result<std::string> validateTweakKey(std::string_view key);

// The value name: not one that starts programs (Shell, Userinit, Load, Run, Debugger,
// SCRNSAVE.EXE...) or that a module of ours manages (the wallpaper, light/dark, the taskbar).
[[nodiscard]] Result<void> validateTweakValueName(std::string_view key, std::string_view name);

// String data: short, printable, no network path or URL, no environment variables, nothing that
// names a program, script or library.
[[nodiscard]] Result<void> validateTweakString(std::string_view data);

// The allow-listed keys (for docs and the install prompt).
[[nodiscard]] std::span<const std::string_view> tweakAllowedKeys() noexcept;

} // namespace rmw::domain

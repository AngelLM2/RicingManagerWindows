#pragma once

#include "rmw/domain/Rice.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Slint source of one bar, generated from its zones (ADR-0008: widgets are .slint components
// composed into a generated root and compiled by the interpreter). Each item becomes a "slot" with
// its own properties and callbacks on the root — "w3-text", "w3-clicked" — so two clocks with
// different formats never share state, and providers address an item by its prefix.
namespace rmw::bar
{

struct WidgetProperty
{
    std::string_view name; // in the widget component
    std::string_view type; // Slint type: "string", "bool", "[WindowItem]"...
};

// A widget the bar can show: built-ins now, plugin widgets later (docs/12). Every widget
// component also takes `foreground: color` and `vertical: bool` from the bar.
struct WidgetKind
{
    std::string_view id;        // as written in the rice: "clock"
    std::string_view component; // the component in the widget sources: "RmwClock"
    std::span<const WidgetProperty> properties;
    std::span<const std::string_view> callbacks; // no arguments, e.g. "clicked"
};

struct Slot
{
    std::string prefix; // "w3"
    const domain::BarItem* item = nullptr;
    const WidgetKind* kind = nullptr;
};

struct GeneratedBar
{
    std::string source;               // widget sources + the generated root component "Bar"
    std::vector<Slot> slots;          // in zone order: left, center, right
    std::vector<std::string> unknown; // item ids no widget handles (left out)
};

// `widgetSources` defines every component and struct the kinds refer to. The root has properties
// "bar-background" (brush), "bar-radius" (length) and "bar-foreground" (color).
[[nodiscard]] GeneratedBar generateBarSource(const domain::BarConfig& bar, std::span<const WidgetKind> kinds,
                                             std::string_view widgetSources);

} // namespace rmw::bar

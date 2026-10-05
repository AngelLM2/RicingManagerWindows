#include "rmw/bar/WidgetSource.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{

using rmw::bar::checkWidgetSource;
using rmw::bar::WidgetSourceOptions;

constexpr std::string_view kNowPlaying = R"slint(
// A plugin widget: what is playing, with a play/pause button.
import { Button } from "std-widgets.slint";

export struct Line {
    text: string,
    bold: bool,
}

component Icon inherits Image {
    width: 16px;
    height: 16px;
}

/* the widget the manifest names */
export component NowPlaying inherits HorizontalLayout {
    in property <color> foreground;
    in property <bool> vertical;
    in property <string> title;
    in-out property <bool> playing;
    in property <[Line]> lines;
    out property <int> count: lines.length;
    callback toggle();
    callback seek(int);
    spacing: 6px;
    Icon {
        source: root.playing ? @image-url("icons/pause.svg") : @image-url("icons/play.svg");
    }
    Text {
        text: "\{root.title} (\{root.count})";
        color: root.foreground;
        animate color { duration: 150ms; }
    }
    for line[i] in root.lines: Text {
        text: line.text;
        font-weight: line.bold ? 700 : 400;
    }
    for i in 3: Rectangle { width: 2px; background: #ff000080; }
    TouchArea {
        clicked => { root.toggle(); }
    }
}
)slint";

WidgetSourceOptions options(std::vector<std::string>* resolved = nullptr)
{
    WidgetSourceOptions o;
    o.prefix = "Rmwp7";
    o.component = "NowPlaying";
    o.resolveImage = [resolved](std::string_view path) -> rmw::Result<std::string> {
        if (resolved != nullptr)
        {
            resolved->emplace_back(path);
        }
        if (path == "missing.png")
        {
            return rmw::fail(rmw::ErrorCode::NotFound, "no such file in the plugin");
        }
        return std::string("C:/plugins/now/") + std::string(path);
    };
    return o;
}

std::string rejectReason(std::string_view source, const WidgetSourceOptions& o = options())
{
    const auto checked = checkWidgetSource(source, "widget.slint", o);
    return checked ? std::string() : checked.error().message();
}

// A tiny valid widget around `body`.
std::string widget(std::string_view body)
{
    return std::string("export component NowPlaying inherits Rectangle {\n") + std::string(body) + "\n}\n";
}

TEST(WidgetSource, AcceptsARealWidgetAndRewritesIt)
{
    std::vector<std::string> resolved;
    const auto checked = checkWidgetSource(kNowPlaying, "widget.slint", options(&resolved));
    ASSERT_TRUE(checked.has_value()) << checked.error().message();
    const std::string& out = checked->source;

    EXPECT_EQ(checked->component, "Rmwp7-NowPlaying");
    EXPECT_EQ(checked->renamed("Line"), "Rmwp7-Line");
    EXPECT_EQ(checked->renamed("Text"), "Text"); // not the plugin's
    EXPECT_EQ(checked->stdWidgets, std::vector<std::string>{"Button"});
    EXPECT_EQ(resolved, (std::vector<std::string>{"icons/pause.svg", "icons/play.svg"}));

    EXPECT_NE(out.find("export component Rmwp7-NowPlaying inherits HorizontalLayout"), std::string::npos)
        << out;
    EXPECT_NE(out.find("<[Rmwp7-Line]>"), std::string::npos) << out;
    EXPECT_NE(out.find("Rmwp7-Icon {"), std::string::npos) << out;
    EXPECT_NE(out.find(R"(@image-url("C:/plugins/now/icons/play.svg"))"), std::string::npos) << out;
    EXPECT_NE(out.find(R"x("\{root.title} (\{root.count})")x"), std::string::npos) << out;
    EXPECT_NE(out.find("line.text"), std::string::npos) << out;
    EXPECT_EQ(out.find("import"), std::string::npos) << out; // hoisted by the generator
    EXPECT_EQ(out.find("//"), std::string::npos) << out;     // comments are gone
    EXPECT_EQ(out.find("manifest names"), std::string::npos) << out;

    const auto& in = checked->interface.inProperties;
    EXPECT_EQ(in, (std::vector<std::string>{"foreground", "vertical", "title", "playing", "lines"}));
    EXPECT_EQ(checked->interface.callbacks, (std::vector<std::string>{"toggle", "seek"}));
}

TEST(WidgetSource, KeepsEveryTokenOnItsLine)
{
    // So Slint's messages point at the plugin's own lines, though comments and imports are gone.
    const auto checked = checkWidgetSource(kNowPlaying, "widget.slint", options());
    ASSERT_TRUE(checked.has_value()) << checked.error().message();
    const auto lineOf = [](std::string_view text, std::string_view what) {
        const auto at = text.find(what);
        return at == std::string_view::npos
                   ? std::size_t{0}
                   : static_cast<std::size_t>(std::ranges::count(text.substr(0, at), '\n')) + 1;
    };
    for (const auto& [in, out] :
         {std::pair<std::string_view, std::string_view>{"export struct Line", "export struct Rmwp7-Line"},
          {"component Icon", "component Rmwp7-Icon"},
          {"export component NowPlaying", "export component Rmwp7-NowPlaying"},
          {"animate color", "animate color"},
          {"for i in 3", "for i in 3"},
          {"TouchArea", "TouchArea"}})
    {
        ASSERT_NE(lineOf(kNowPlaying, in), 0u) << in;
        EXPECT_EQ(lineOf(checked->source, out), lineOf(kNowPlaying, in)) << in << "\n" << checked->source;
    }
}

TEST(WidgetSource, FilesStayOutUnlessTheyAreImagesOfThePlugin)
{
    for (const std::string_view source : {
             R"(import "font.ttf";)",
             R"(import { Thing } from "other.slint";)",
             R"(import { Thing } from "C:/x/std-widgets.slint";)",
             R"(import { Button as B } from "std-widgets.slint";)",
             R"(export { NowPlaying } from "other.slint";)",
             R"(export * from "other.slint";)",
         })
    {
        EXPECT_NE(rejectReason(std::string(source) + "\n" + widget("")), "") << source;
    }
    for (const std::string_view image : {
             R"(@image-url("C:/Windows/x.png"))",
             R"(@image-url("../x.png"))",
             R"(@image-url("\\\\server\\share\\x.png"))",
             R"(@image-url("//server/share/x.png"))",
             R"(@image-url("x.exe"))",
             R"(@image-url("missing.png"))",
             R"(@image-url("a" + ".png"))",
             R"(@image-url("\{root.name}.png"))",
             R"(@image-url("x.png", nine-slice(1)))",
             R"(@image-url(root.path))",
         })
    {
        EXPECT_NE(rejectReason(widget(std::string("Image { source: ") + std::string(image) + "; }")), "")
            << image;
    }
}

TEST(WidgetSource, WhatWouldHurtTheBarIsRejected)
{
    for (const std::string_view body : {
             "Timer { interval: 1ms; triggered => {} }",
             "PopupWindow { }",
             "ContextMenuArea { }",
             "animate x { duration: 1s; iteration-count: -1; }",
             "for i in 100000: Rectangle { }",
             "RmwClock { }",
             "rmw_thing := Rectangle { }",
             "@children-of-something",
             "Text { text: @tr(\"x\"); } @unknown",
             R"(Text { text: "\{ "nested" }"; })",
             R"(Text { text: "\{ {a: 1}.a }"; })",
             "Text { text: \"line\nbreak\"; }",
             "x: 1 ⟶ 2;",
         })
    {
        EXPECT_NE(rejectReason(widget(body)), "") << body;
    }
    EXPECT_NE(rejectReason("export component W inherits Window { }"), "");
    EXPECT_NE(rejectReason("export component NowPlaying inherits Rectangle { "), ""); // unbalanced
    EXPECT_NE(rejectReason("NowPlaying := Rectangle { }"), "");                       // old top level
}

TEST(WidgetSource, LimitsHold)
{
    std::string deep = "export component NowPlaying inherits Rectangle { x: ";
    for (int i = 0; i < 40; ++i)
    {
        deep += "(";
    }
    deep += "1";
    for (int i = 0; i < 40; ++i)
    {
        deep += ")";
    }
    deep += "; }";
    EXPECT_NE(rejectReason(deep), "");

    std::string big = widget("");
    big.append(rmw::bar::kWidgetSourceMaxBytes, ' ');
    EXPECT_NE(rejectReason(big), "");

    EXPECT_NE(rejectReason(widget("Text { text: \"\xC3\x28\"; }")), "");        // invalid UTF-8
    EXPECT_EQ(rejectReason(widget("Text { text: \"\xE2\x96\xB6 ok\"; }")), ""); // "▶" inside a string
}

TEST(WidgetSource, CommentsCannotHideAnything)
{
    // Whatever a comment holds is dropped before compiling, so it cannot smuggle code in.
    const auto checked = checkWidgetSource(widget(R"(
        // import "font.ttf";
        /* @image-url("C:/Windows/x.png") */
        Text { text: "/* not a comment */"; }
    )"),
                                           "widget.slint", options());
    ASSERT_TRUE(checked.has_value()) << checked.error().message();
    EXPECT_EQ(checked->source.find("font.ttf"), std::string::npos);
    EXPECT_EQ(checked->source.find("C:/Windows"), std::string::npos);
    EXPECT_NE(checked->source.find("\"/* not a comment */\""), std::string::npos);
}

TEST(WidgetSource, TheManifestComponentMustExist)
{
    auto o = options();
    o.component = "Missing";
    EXPECT_NE(rejectReason(widget(""), o), "");
    o.component = "NowPlaying";
    o.verticalComponent = "Vertical";
    EXPECT_NE(rejectReason(widget(""), o), "");
    o.prefix = "P1"; // the bar's own prefix
    EXPECT_NE(rejectReason(widget(""), o), "");
}

TEST(WidgetSource, DuplicatesAndUnderscoresAreTheSameName)
{
    EXPECT_NE(rejectReason(widget("") + "struct my_item { a: int }\nstruct my-item { b: int }\n"), "");
    const auto checked = checkWidgetSource(widget("in property <my_item> x;") + "struct my-item { a: int }\n",
                                           "widget.slint", options());
    ASSERT_TRUE(checked.has_value()) << checked.error().message();
    EXPECT_NE(checked->source.find("<Rmwp7-my-item>"), std::string::npos) << checked->source;
}

} // namespace

#include "rmw/bar/Bridge.hpp"
#include "rmw/bar/Tray.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

namespace
{

using namespace rmw::bar::tray;

constexpr std::size_t kFullSize = 964; // what shell32 sends on Windows 10 and 11

// A Shell_NotifyIcon payload as shell32 lays it out.
class Payload
{
public:
    explicit Payload(Command command, std::size_t size = kFullSize)
        : m_bytes(size)
    {
        put(0, kSignature);
        put(4, static_cast<std::uint32_t>(command));
    }

    Payload& owner(std::uint32_t value) { return put(12, value); }
    Payload& id(std::uint32_t value) { return put(16, value); }
    Payload& flags(std::uint32_t value) { return put(20, value); }
    Payload& callback(std::uint32_t value) { return put(24, value); }
    Payload& icon(std::uint32_t value) { return put(28, value); }
    Payload& state(std::uint32_t state, std::uint32_t mask) { return put(288, state).put(292, mask); }
    Payload& version(std::uint32_t value) { return put(808, value); }
    Payload& tip(std::u16string_view text)
    {
        std::memcpy(m_bytes.data() + 32, text.data(), text.size() * sizeof(char16_t));
        return *this;
    }
    Payload& guid(std::uint8_t first)
    {
        m_bytes[944] = std::byte{first};
        return *this;
    }
    Payload& put(std::size_t offset, std::uint32_t value)
    {
        std::memcpy(m_bytes.data() + offset, &value, sizeof(value));
        return *this;
    }

    [[nodiscard]] Update decoded() const
    {
        auto update = decodeUpdate(m_bytes);
        EXPECT_TRUE(update.has_value());
        return update.value_or(Update{});
    }
    [[nodiscard]] std::span<const std::byte> bytes() const { return m_bytes; }

private:
    std::vector<std::byte> m_bytes;
};

Update add(std::uint32_t owner, std::uint32_t id, std::u16string_view tip, std::uint32_t icon = 0x100)
{
    return Payload(Command::Add)
        .owner(owner)
        .id(id)
        .flags(kFlagMessage | kFlagIcon | kFlagTip)
        .callback(0x8001)
        .icon(icon)
        .tip(tip)
        .decoded();
}

TEST(Tray, DecodesWhatShellNotifyIconSends)
{
    const Update update = Payload(Command::Add)
                              .owner(0x1234)
                              .id(7)
                              .flags(kFlagMessage | kFlagIcon | kFlagTip | kFlagGuid | kFlagShowTip)
                              .callback(0x8001)
                              .icon(0xABCD)
                              .tip(u"Discord")
                              .guid(0x42)
                              .decoded();
    EXPECT_EQ(update.command, Command::Add);
    EXPECT_EQ(update.owner, 0x1234u);
    EXPECT_EQ(update.id, 7u);
    EXPECT_EQ(update.callback, 0x8001u);
    EXPECT_EQ(update.icon, 0xABCDu);
    EXPECT_EQ(update.tip, u"Discord");
    ASSERT_TRUE(update.guid.has_value());
    EXPECT_EQ((*update.guid)[0], 0x42);

    const Update version = Payload(Command::SetVersion).owner(1).id(2).version(4).decoded();
    EXPECT_EQ(version.command, Command::SetVersion);
    EXPECT_EQ(version.version, 4u);
}

TEST(Tray, RefusesWhatIsNotANotifyIconCall)
{
    EXPECT_FALSE(decodeUpdate({}).has_value());
    EXPECT_FALSE(decodeUpdate(Payload(Command::Add, 100).bytes()).has_value()); // shorter than the tip
    Payload wrong(Command::Add);
    wrong.put(0, 0x12345678);
    EXPECT_FALSE(decodeUpdate(wrong.bytes()).has_value());
    Payload unknown(Command::Add);
    unknown.put(4, 9);
    EXPECT_FALSE(decodeUpdate(unknown.bytes()).has_value());

    // A tip that fills all 128 characters, with no terminator: read up to its end, no further.
    Payload longTip(Command::Add);
    longTip.tip(std::u16string(128, u'x'));
    longTip.put(288, 0x41414141); // what follows is not text
    EXPECT_EQ(longTip.decoded().tip.size(), 128u);

    // Only the fields up to the tip: still an update, without the rest.
    const auto minimal = decodeUpdate(Payload(Command::Add, 288).owner(5).bytes());
    ASSERT_TRUE(minimal.has_value());
    EXPECT_EQ(minimal->owner, 5u);
    EXPECT_FALSE(minimal->guid.has_value());
}

TEST(Tray, ModelFollowsAddModifyDelete)
{
    Model model;
    const auto added = model.apply(add(0x10, 1, u"One"));
    EXPECT_EQ(added.change, Model::Change::Added);
    EXPECT_TRUE(added.iconChanged);
    const auto second = model.apply(add(0x20, 1, u"Two"));
    EXPECT_NE(second.number, added.number);
    ASSERT_EQ(model.icons().size(), 2u);
    EXPECT_EQ(model.icons()[0].tip, u"One"); // the order apps added them

    const auto modified =
        model.apply(Payload(Command::Modify).owner(0x10).id(1).flags(kFlagTip).tip(u"One, again").decoded());
    EXPECT_EQ(modified.change, Model::Change::Changed);
    EXPECT_EQ(modified.number, added.number);
    EXPECT_FALSE(modified.iconChanged);
    EXPECT_EQ(model.find(added.number)->tip, u"One, again");
    EXPECT_EQ(model.find(added.number)->callback, 0x8001u); // kept

    const auto hidden = model.apply(Payload(Command::Modify)
                                        .owner(0x10)
                                        .id(1)
                                        .flags(kFlagState)
                                        .state(kStateHidden, kStateHidden)
                                        .decoded());
    EXPECT_EQ(hidden.change, Model::Change::Changed);
    EXPECT_TRUE(model.find(added.number)->hidden);

    const auto version = model.apply(Payload(Command::SetVersion).owner(0x10).id(1).version(4).decoded());
    EXPECT_EQ(version.change, Model::Change::Changed);
    EXPECT_EQ(model.find(added.number)->version, 4u);

    const auto removed = model.apply(Payload(Command::Delete).owner(0x10).id(1).decoded());
    EXPECT_EQ(removed.change, Model::Change::Removed);
    EXPECT_EQ(removed.number, added.number);
    EXPECT_EQ(model.find(added.number), nullptr);
    EXPECT_EQ(model.apply(Payload(Command::Delete).owner(0x99).id(1).decoded()).change, Model::Change::None);
}

TEST(Tray, IconsAddedBeforeUsAppearWhenTheyChange)
{
    Model model;
    // Only a tip: nothing to draw yet.
    EXPECT_EQ(
        model.apply(Payload(Command::Modify).owner(0x10).id(3).flags(kFlagTip).tip(u"t").decoded()).change,
        Model::Change::None);
    // With an icon: shown.
    const auto shown = model.apply(Payload(Command::Modify)
                                       .owner(0x10)
                                       .id(3)
                                       .flags(kFlagIcon | kFlagTip)
                                       .icon(0x77)
                                       .tip(u"t")
                                       .decoded());
    EXPECT_EQ(shown.change, Model::Change::Added);
}

TEST(Tray, GuidIconsFollowTheirGuid)
{
    Model model;
    const auto first = model.apply(Payload(Command::Add)
                                       .owner(0x10)
                                       .id(0)
                                       .flags(kFlagIcon | kFlagGuid | kFlagMessage)
                                       .icon(1)
                                       .callback(0x8000)
                                       .guid(9)
                                       .decoded());
    // The same GUID from another window is the same icon.
    const auto moved = model.apply(
        Payload(Command::Modify).owner(0x20).id(0).flags(kFlagIcon | kFlagGuid).icon(2).guid(9).decoded());
    EXPECT_EQ(moved.number, first.number);
    EXPECT_EQ(model.icons().size(), 1u);
    EXPECT_EQ(model.icons()[0].owner, 0x20u);
}

TEST(Tray, TheModelHasALimitAndDropsDeadOwners)
{
    Model model;
    for (std::uint32_t i = 0; i < Model::kMaxIcons + 10; ++i)
    {
        model.apply(add(0x1000 + i, 1, u"x"));
    }
    EXPECT_EQ(model.icons().size(), Model::kMaxIcons);
    const auto removed = model.removeIf([](std::uint32_t owner) { return owner % 2 == 0; });
    EXPECT_EQ(removed.size(), Model::kMaxIcons / 2);
    EXPECT_EQ(model.icons().size(), Model::kMaxIcons / 2);
}

TEST(Tray, ClicksSpeakEachVersion)
{
    Icon old;
    old.id = 5;
    old.callback = 0x8001;
    const auto left = postsFor(old, Gesture::LeftClick, 100, 200);
    ASSERT_EQ(left.size(), 2u); // down, up: version 0 has no NIN_SELECT
    EXPECT_EQ(left[0], (Post{5, 0x0201}));
    EXPECT_EQ(left[1], (Post{5, 0x0202}));
    EXPECT_EQ(postsFor(old, Gesture::RightClick, 0, 0).size(), 2u);
    EXPECT_TRUE(postsFor(old, Gesture::HoverEnd, 0, 0).empty());

    Icon three = old;
    three.version = 3;
    const auto selected = postsFor(three, Gesture::LeftClick, 0, 0);
    ASSERT_EQ(selected.size(), 3u);
    EXPECT_EQ(selected[2], (Post{5, 0x0400}));                                       // NIN_SELECT
    EXPECT_EQ(postsFor(three, Gesture::RightClick, 0, 0).back(), (Post{5, 0x007B})); // WM_CONTEXTMENU

    Icon four = old;
    four.version = 4;
    const auto menu = postsFor(four, Gesture::RightClick, 100, 200);
    ASSERT_EQ(menu.size(), 3u);
    // wParam: the anchor (x low, y high); lParam: the event, the icon's id high.
    EXPECT_EQ(menu[2].wParam, static_cast<std::uintptr_t>((200 << 16) | 100));
    EXPECT_EQ(menu[2].lParam, static_cast<std::intptr_t>((5 << 16) | 0x007B));
    // A point left of the primary monitor: x is negative, as GET_X_LPARAM reads it back.
    const auto negative = postsFor(four, Gesture::LeftClick, -10, 20);
    EXPECT_EQ(static_cast<std::int16_t>(negative[0].wParam & 0xFFFF), -10);

    // Its own popup instead of the standard tip.
    const auto hover = postsFor(four, Gesture::HoverStart, 0, 0);
    ASSERT_EQ(hover.size(), 2u);
    EXPECT_EQ(hover[1].lParam & 0xFFFF, 0x0406); // NIN_POPUPOPEN
    EXPECT_EQ(postsFor(four, Gesture::HoverEnd, 0, 0).size(), 1u);
    four.tip = u"tip";
    EXPECT_FALSE(barShowsTip(four));
    four.showTip = true;
    EXPECT_TRUE(barShowsTip(four));
    EXPECT_EQ(postsFor(four, Gesture::HoverStart, 0, 0).size(), 1u);

    Icon silent = old;
    silent.callback = 0; // no NIF_MESSAGE: nothing to send
    EXPECT_TRUE(postsFor(silent, Gesture::LeftClick, 0, 0).empty());
}

TEST(Tray, RectQueries)
{
    for (const std::size_t size : {std::size_t{40}, std::size_t{36}})
    {
        std::vector<std::byte> bytes(size);
        const std::size_t owner = size == 40 ? 16 : 12;
        const auto put = [&](std::size_t at, std::uint32_t v) { std::memcpy(bytes.data() + at, &v, 4); };
        put(0, kSignature);
        put(4, 2);
        put(owner, 0x1234);
        put(owner + 4, 7);
        const auto query = decodeRectQuery(bytes);
        ASSERT_TRUE(query.has_value()) << size;
        EXPECT_TRUE(query->size);
        EXPECT_EQ(query->owner, 0x1234u);
        EXPECT_EQ(query->id, 7u);
        EXPECT_FALSE(query->guid.has_value());
        put(4, 3);
        EXPECT_FALSE(decodeRectQuery(bytes).has_value());
    }
    EXPECT_FALSE(decodeRectQuery(std::vector<std::byte>(20)).has_value());
    EXPECT_EQ(packPoint(-5, 10), static_cast<std::int32_t>((10U << 16) | 0xFFFBU));
}

TEST(Tray, BridgeCarriesTheTrayProvider)
{
    namespace bridge = rmw::bar::bridge;
    const auto watch = bridge::decode(bridge::encode(bridge::Watch{{"tray"}}));
    ASSERT_TRUE(watch.has_value()) << watch.error().message();
    EXPECT_EQ(std::get<bridge::Watch>(*watch).providers, std::vector<std::string>{"tray"});
    const auto provide = bridge::decode(
        bridge::encode(bridge::Provide{"tray", rmw::domain::Value{rmw::domain::Value::Array{}}}));
    ASSERT_TRUE(provide.has_value());
    EXPECT_EQ(std::get<bridge::Provide>(*provide).name, "tray");
    EXPECT_FALSE(bridge::decode(R"({"t":"watch","providers":["media"]})").has_value());
    EXPECT_FALSE(bridge::decode(R"({"t":"provide","name":"system.cpu","value":1})").has_value());
}

} // namespace

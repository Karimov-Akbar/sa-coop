// The F2 window (PlayersMenu.h). Our own money is read from the game, the others' is what their games said last (Money.cpp).
#include "pages/players-menu/PlayersMenu.h"

#include <algorithm>
#include <cstdlib>
#include <string>

#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/money/Money.h"
#include "features/teleport/Teleport.h"
#include "features/voice/Voice.h"
#include "features/weapon-gift/WeaponGift.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/input/Input.h"
#include "shared/ui/renderer/Renderer.h"
#include "shared/ui/widgets/Widgets.h"

namespace coop::ui {

namespace {
using w::kSaBlue, w::kSaMuted, w::kSaGreen, w::kSaRed, w::kSaLine;
constexpr uint32_t kText = 0xFFE6E9EF;

bool g_visible = false;
uint8_t g_giveTo = 0xFF;  // the player whose gift is being typed
bool g_giveGun = false;   // a weapon of ours, not money
int g_gun = 0;            // which one
std::wstring g_amount;
std::string g_note;  // what the last action did, shown a while
bool g_noteError = false;
uint64_t g_noteUntil = 0;

void Note(const std::string& text, bool error) { g_note = text, g_noteError = error, g_noteUntil = NowMs() + 6000; }

// $1 234 567 (ru), $1,234,567 (en)
std::string MoneyText(int32_t m) {
    std::string digits = std::to_string(m < 0 ? -(int64_t)m : (int64_t)m), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += TR(" ", ",");
        out += digits[i];
    }
    return (m < 0 ? "-$" : "$") + out;
}

bool Hovered(float x, float y, float w, float h) {
    auto& st = In().State();
    return st.mouseX >= x && st.mouseX < x + w && st.mouseY >= y && st.mouseY < y + h;
}

// the game's own money sign (the HUD's pricedown $) on a small square: true when clicked
bool MoneyButton(float x, float y, float size, bool enabled) {
    auto& g = Gfx();
    bool hover = enabled && Hovered(x, y, size, size);
    g.Rect(x, y, size, size, hover ? 0xFF16202A : 0xFF0C1117);
    g.RectOutline(x, y, size, size, hover ? kSaBlue : kSaLine, std::max(1.0f, g.Scale()));
    float th = size * 0.68f;
    g.GameText(GAME_FONT_PRICEDOWN, x + size * 0.5f, y + (size - th) * 0.5f, "$", th,
               !enabled ? 0xFF2E4A28 : hover ? 0xFF8CD47A : kSaGreen, TEXT_CENTER);
    return hover && In().State().lmbPressed;
}

// a pistol of three bars on a small square: true when clicked
bool GunButton(float x, float y, float size, bool enabled) {
    auto& g = Gfx();
    bool hover = enabled && Hovered(x, y, size, size);
    g.Rect(x, y, size, size, hover ? 0xFF16202A : 0xFF0C1117);
    g.RectOutline(x, y, size, size, hover ? kSaBlue : kSaLine, std::max(1.0f, g.Scale()));
    uint32_t c = !enabled ? 0xFF3A4450 : hover ? 0xFFFFFFFF : 0xFFC8D0DA;
    float u = size / 16.0f;
    g.Rect(x + 3 * u, y + 5 * u, 10 * u, 3 * u, c);      // the barrel
    g.Rect(x + 3.5f * u, y + 8 * u, 3 * u, 4.5f * u, c);  // the grip
    g.Rect(x + 6.5f * u, y + 8 * u, 2 * u, 1.5f * u, c);  // the trigger
    return hover && In().State().lmbPressed;
}

// an arrow on a small square, to the right (/tp: over to that player) or to the left (/tphere: to us): true when clicked
bool GoButton(float x, float y, float size, bool enabled, bool left) {
    auto& g = Gfx();
    bool hover = enabled && Hovered(x, y, size, size);
    g.Rect(x, y, size, size, hover ? 0xFF16202A : 0xFF0C1117);
    g.RectOutline(x, y, size, size, hover ? kSaBlue : kSaLine, std::max(1.0f, g.Scale()));
    uint32_t c = !enabled ? 0xFF3A4450 : hover ? 0xFFFFFFFF : 0xFFC8D0DA;
    float u = size / 16.0f;
    auto bar = [&](float bx, float by, float bw, float bh) { g.Rect(x + (left ? 16 - bx - bw : bx) * u, y + by * u, bw * u, bh * u, c); };
    bar(3, 7, 7, 2);        // the shaft
    bar(9, 4, 2, 8);        // the head, narrowing
    bar(11, 5.5f, 1.5f, 5);
    bar(12.5f, 7, 1.5f, 2);
    return hover && In().State().lmbPressed;
}

std::wstring Rounds(int ammo) { return std::to_wstring(std::max(ammo, 0)); }

void Submit() {
    int64_t amount = std::min<int64_t>(g_amount.empty() ? 0 : _wtoi64(g_amount.c_str()), INT32_MAX);
    std::string error;
    if (g_giveGun) {
        if (!weapon_gift::Give(g_giveTo, g_gun, (int)amount, error)) return Note(error, true);
        Note(Format(TR("Вы передали игроку %s: %s.", "You gave %s: %s."), Net().PeerName(g_giveTo).c_str(), weapon_gift::Name(g_gun)),
             false);
    } else {
        if (!money::Give(g_giveTo, (int32_t)amount, error)) return Note(error, true);
        Note(Format(TR("Вы передали %s игроку %s.", "You gave %s to %s."), MoneyText((int32_t)amount).c_str(),
                    Net().PeerName(g_giveTo).c_str()),
             false);
    }
    g_giveTo = 0xFF;
    g_amount.clear();
}
}  // namespace

void SetPlayersMenuVisible(bool visible) {
    if (visible == g_visible) return;
    g_visible = visible;
    g_giveTo = 0xFF;
    g_giveGun = false;
    g_amount.clear();
    g_noteUntil = 0;
    if (visible && TheChat().IsOpen()) TheChat().Close();
    if (!visible && core::ConnectMenuVisible()) return;  // F9 opened over it keeps the mouse and the keys
    In().SetKeyboardCapture(visible);
    In().SetMouseCapture(visible);
}

bool IsPlayersMenuVisible() { return g_visible; }

void TestGift(uint8_t to, const std::wstring& amount) {
    if (!g_visible) TogglePlayersMenu();
    if (g_visible) g_giveTo = to, g_giveGun = false, g_amount = amount;
}

void TogglePlayersMenu() {
    if (g_visible) return SetPlayersMenuVisible(false);
    if (Net().Online() && core::IsPlayerReady() && !game::MenuActive() && !core::ConnectMenuVisible()) SetPlayersMenuVisible(true);
}

void RenderPlayersMenu() {
    if (!g_visible) return;
    auto& net = Net();
    if (!net.Online() || !core::IsPlayerReady() || game::MenuActive() || core::ConnectMenuVisible()) return SetPlayersMenuVisible(false);
    if (g_giveTo != 0xFF && !net.Peers().count(g_giveTo)) g_giveTo = 0xFF;
    auto& g = Gfx();
    float s = g.Scale();
    float lh = g.LineHeight(FONT_NORMAL), sh = g.LineHeight(FONT_SMALL), ch = g.LineHeight(FONT_MENU_SMALL),
          mh = g.LineHeight(FONT_MENU);
    float rowH = lh + 16 * s, gap = 6 * s, room = mh * 1.6f;  // room: the space beside a menu item's name
    int rows = 1 + (int)net.Peers().size();
    // the columns as wide as their longest texts (the menu font's spaced capitals took twice a guessed share), counted
    // from the right: the gift buttons, the ping, the money, the role; the nickname has the rest
    float bs = rowH - 12 * s, colGap = 32 * s, pad = 32 * s, bgap = 8 * s;
    // /tp and /tphere by a click, with several to choose from (the user's word of 02.11 and 06.11); the commands' own rules
    // hold. With several the rows show the numbers those commands take.
    bool many = net.Peers().size() > 1, tp = many && (net.IsHost() || net.PartnerTeleport());
    auto label = [many](uint8_t id, const std::string& name) { return many ? Format("[%d] %s", id, name.c_str()) : name; };
    float actW = (tp ? 4 : 2) * bs + (tp ? 3 : 1) * bgap;
    float pingW = std::max(g.TextWidth(FONT_SMALL, TR("999 мс", "999 ms")), g.TextWidth(FONT_MENU_SMALL, TR("Пинг", "Ping")));
    float moneyW = std::max(g.TextWidth(FONT_NORMAL, MoneyText(-99999999)), g.TextWidth(FONT_MENU_SMALL, TR("Деньги", "Money")));
    float roleW = std::max({g.TextWidth(FONT_MENU_SMALL, TR("напарник", "partner")), g.TextWidth(FONT_MENU_SMALL, TR("хост", "host")),
                            g.TextWidth(FONT_MENU_SMALL, TR("Роль", "Role"))});
    float nameW = std::max(g.TextWidth(FONT_NORMAL, label(net.MyId(), Cfg().nickname) + TR(" (вы)", " (you)")), g.TextWidth(FONT_MENU_SMALL, TR("Ник", "Nickname")));
    for (auto& [id, p] : net.Peers()) nameW = std::max(nameW, g.TextWidth(FONT_NORMAL, label(id, p.name)));
    float need = 14 * s + nameW + roleW + moneyW + pingW + actW + 4 * colGap;
    float W = std::min(std::max(820 * s, need + 2 * pad), g.Width() - 32 * s), cw = W - 2 * pad;
    std::vector<weapon_gift::Owned> guns = g_giveTo != 0xFF && g_giveGun ? weapon_gift::Ours() : std::vector<weapon_gift::Owned>{};
    float giftH = g_giveTo == 0xFF ? 0.0f : 12 * s + sh + 8 * s + rowH + (guns.empty() ? 0.0f : 8 * s + rowH);
    float H = 24 * s + mh + 18 * s + ch + 8 * s + rows * (rowH + gap) + giftH + 14 * s + sh + 14 * s + rowH + 20 * s;
    float x = (g.Width() - W) * 0.5f, y = std::max(16 * s, (g.Height() - H) * 0.45f);  // under the contest's board on top
    float cx = x + pad, cy = y + 24 * s;
    w::BeginFrame();
    g.Rect(x, y, W, H, 0xF0000000);
    g.RectOutline(x, y, W, H, kSaLine, std::max(1.0f, s));
    g.Text(FONT_MENU, cx, cy, TR("Игроки", "Players"), kSaBlue, TEXT_SHADOW);
    g.Text(FONT_SMALL, cx + cw, cy + (mh - sh) * 0.5f, net.ServerName(), kSaMuted, TEXT_RIGHT | TEXT_SHADOW);
    cy += mh + 18 * s;

    float colAct = cx + cw - actW, colPing = colAct - colGap, colMoney = colPing - pingW - colGap,
          colRole = colMoney - moneyW - colGap - roleW;  // right edges of the ping and the money, left of the others
    g.Text(FONT_MENU_SMALL, cx + 14 * s, cy, TR("Ник", "Nickname"), kSaMuted);
    g.Text(FONT_MENU_SMALL, colRole, cy, TR("Роль", "Role"), kSaMuted);
    g.Text(FONT_MENU_SMALL, colMoney, cy, TR("Деньги", "Money"), kSaMuted, TEXT_RIGHT);
    g.Text(FONT_MENU_SMALL, colPing, cy, TR("Пинг", "Ping"), kSaMuted, TEXT_RIGHT);
    cy += ch + 8 * s;
    auto row = [&](uint8_t id, const std::string& name, bool isHost, uint32_t ping, bool me) {
        g.Rect(cx, cy, cw, rowH, 0xFF090C10);
        float ty = cy + (rowH - lh) * 0.5f;
        bool talking = me ? VoiceIsTransmitting() : VoiceIsSpeaking(id);
        std::string shown = (talking ? TalkingMark() : std::string()) + label(id, name) + (me ? TR(" (вы)", " (you)") : "");
        g.Text(FONT_NORMAL, cx + 14 * s, ty, shown, me ? 0xFFFFFFFF : kText, TEXT_SHADOW);
        g.Text(FONT_MENU_SMALL, colRole, cy + (rowH - ch) * 0.5f, isHost ? TR("хост", "host") : TR("напарник", "partner"),
               isHost ? kSaBlue : kSaGreen);
        int32_t m = 0;
        if (me ? (m = game::PlayerInfos()[0].money, true) : money::Of(id, m))
            g.Text(FONT_NORMAL, colMoney, ty, MoneyText(m), kSaGreen, TEXT_RIGHT | TEXT_SHADOW);
        g.Text(FONT_SMALL, colPing, cy + (rowH - sh) * 0.5f, Format(TR("%u мс", "%u ms"), ping), kSaMuted, TEXT_RIGHT);
        if (!me) {
            const RemotePlayer* rp = Players::Instance().Get(id);
            bool inGame = rp && rp->hasState;
            float by = cy + (rowH - bs) * 0.5f;
            bool gun = GunButton(colAct, by, bs, inGame), cash = MoneyButton(colAct + bs + bgap, by, bs, inGame);
            if (tp && GoButton(colAct + 2 * (bs + bgap), by, bs, inGame, false)) teleport::Command("tp", {std::to_string(id)});
            if (tp && GoButton(colAct + 3 * (bs + bgap), by, bs, inGame, true)) teleport::Command("tphere", {std::to_string(id)});
            if (gun || cash) {
                g_giveTo = g_giveTo == id && g_giveGun == gun ? 0xFF : id;
                g_giveGun = gun;
                g_amount.clear();
                if (auto ours = weapon_gift::Ours(); gun && !ours.empty()) g_gun = ours[0].type, g_amount = Rounds(ours[0].ammo);
                if (g_giveTo != 0xFF) w::SetFocus("amount");
            }
        }
        cy += rowH + gap;
    };
    row(net.MyId(), Cfg().nickname, net.IsHost(), net.Rtt(), true);
    for (auto& [id, p] : net.Peers()) row(id, p.name, p.isHost, p.ping, false);

    if (g_giveTo != 0xFF && g_giveGun) {  // which weapon of ours, and how many of its rounds
        if (guns.empty()) guns = weapon_gift::Ours();  // opened in this frame
        cy += 12 * s - gap;
        const char* give = TR("Передать", "Give");
        const char* cancel = TR("Отмена", "Cancel");
        float kx = cx + cw - g.TextWidth(FONT_MENU, cancel), gx = cx + room;
        bool enter = false;
        if (guns.empty()) {
            g.Text(FONT_SMALL, cx, cy, TR("У вас нет оружия, которое можно передать.", "You have no weapon to give."), kSaMuted,
                   TEXT_SHADOW);
            cy += sh + 8 * s;
        } else {
            size_t i = 0;
            while (i < guns.size() && guns[i].type != g_gun) ++i;
            if (i == guns.size()) i = 0, g_gun = guns[0].type, g_amount = Rounds(guns[0].ammo);  // gone meanwhile
            g.Text(FONT_SMALL, cx, cy, Format(TR("Что передать игроку %s", "What to give %s"), net.PeerName(g_giveTo).c_str()), kSaMuted,
                   TEXT_SHADOW);
            cy += sh + 8 * s;
            std::string label = guns[i].whole ? std::string(weapon_gift::Name(guns[i].type))
                                              : Format(TR("%s, патроны: %d", "%s, ammo: %d"), weapon_gift::Name(guns[i].type), guns[i].ammo);
            if (int step = w::Selector("gun", cx, cy, cw, rowH, label)) {
                i = (i + guns.size() + step) % guns.size();
                g_gun = guns[i].type;
                g_amount = Rounds(guns[i].ammo);
            }
            cy += rowH + 8 * s;
            if (!guns[i].whole) {
                float boxW = 300 * s;
                enter = w::TextBox("amount", cx, cy, boxW, rowH, g_amount, 5, false, true, TR("патроны", "rounds"));
                gx = cx + boxW + 24 * s + room;
            }
            if (w::Button("give", gx - room, cy, g.TextWidth(FONT_MENU, give) + 2 * room, rowH, give) || enter) Submit();
        }
        if (g_giveTo != 0xFF && w::Button("cancel", kx - room, cy, g.TextWidth(FONT_MENU, cancel) + 2 * room, rowH, cancel))
            g_giveTo = 0xFF;
        cy += rowH;
    } else if (g_giveTo != 0xFF) {  // how much of ours to give that player
        cy += 12 * s - gap;
        g.Text(FONT_SMALL, cx, cy,
               Format(TR("Сколько передать игроку %s (у вас %s)", "How much to give %s (you have %s)"), net.PeerName(g_giveTo).c_str(),
                      MoneyText(game::PlayerInfos()[0].money).c_str()),
               kSaMuted, TEXT_SHADOW);
        cy += sh + 8 * s;
        float boxW = 300 * s;
        bool enter = w::TextBox("amount", cx, cy, boxW, rowH, g_amount, 9, false, true, TR("сумма", "amount"));
        const char* give = TR("Передать", "Give");
        const char* cancel = TR("Отмена", "Cancel");
        float gx = cx + boxW + 24 * s + room, kx = cx + cw - g.TextWidth(FONT_MENU, cancel);
        if (w::Button("give", gx - room, cy, g.TextWidth(FONT_MENU, give) + 2 * room, rowH, give) || enter) Submit();
        else if (w::Button("cancel", kx - room, cy, g.TextWidth(FONT_MENU, cancel) + 2 * room, rowH, cancel)) g_giveTo = 0xFF;
        cy += rowH;
    }
    cy += 14 * s;
    if (NowMs() < g_noteUntil) g.Text(FONT_SMALL, cx, cy, g_note, g_noteError ? kSaRed : kSaBlue, TEXT_SHADOW);
    cy += sh + 14 * s;
    const char* close = TR("Закрыть", "Close");
    float closeX = cx + cw - g.TextWidth(FONT_MENU, close);
    bool closing = w::Button("close", closeX - room, cy, g.TextWidth(FONT_MENU, close) + 2 * room, rowH, close);
    w::EndFrame();
    w::DrawCursor();
    if (closing) SetPlayersMenuVisible(false);
}

}  // namespace coop::ui

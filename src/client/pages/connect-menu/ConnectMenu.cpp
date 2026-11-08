#include "pages/connect-menu/ConnectMenu.h"

#include <windows.h>

#include <algorithm>

#include "common/api/nat-traversal/NatTraversal.h"
#include "common/config/version/Version.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/session/Flow.h"
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
using w::kSaBlue, w::kSaSlate, w::kSaMuted, w::kSaRed, w::kSaLine, w::kSaGreen;
constexpr uint32_t kText = 0xFFE6E9EF;
constexpr uint32_t kGold = 0xFFE2C063;  // the game's mission-title cream

bool g_visible = false;
bool g_autoShown = false;
int g_tab = 0;  // 0 "connect", 1 "create"
bool g_keysPage = false;  // the mod's keys and the microphone instead of the rest
int g_capture = -1;       // the key of that action is being chosen (the next key pressed)
std::wstring g_nick, g_addr, g_port, g_password;
std::string g_formError;
int g_slotIndex = 0;  // index into the slot list
std::vector<flow::SaveSlotInfo> g_slots;
uint64_t g_slotsTime = 0;
uint64_t g_copiedMs = 0;

void CopyToClipboard(const std::string& utf8) {
    std::wstring text = Utf8ToWide(utf8);
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        else g_copiedMs = NowMs();
    }
    CloseClipboard();
}

// the save slots and, after them, a new game (the host plays its prologue alone, partners join at the bike)
void RefreshSlots() {
    g_slots = flow::ListSaveSlots();
    flow::SaveSlotInfo fresh;
    fresh.slot = -1;
    fresh.exists = true;
    fresh.title = TR("Новая игра", "New game");
    g_slots.push_back(fresh);
    g_slotsTime = NowMs();
}

void LoadFields() {
    auto& cfg = Cfg();
    g_nick = Widen(cfg.nickname);
    g_addr = Widen(cfg.host);
    g_port = std::to_wstring(cfg.port);
    g_password = Widen(cfg.password);
    g_tab = cfg.wantsHost ? 1 : 0;
    g_formError.clear();
    RefreshSlots();
    // preselect the first save; without any, the new game
    g_slotIndex = 0;
    for (size_t i = 0; i < g_slots.size(); ++i)
        if (g_slots[i].exists) {
            g_slotIndex = (int)i;
            break;
        }
}

bool ApplyNick() {
    std::string nick = SanitizeNickname(Narrow(g_nick));
    if (nick.size() < 2) {
        g_formError = TR("В нике должно быть от 2 до 20 символов (буквы, цифры, знаки _-.[]).",
                         "A nickname needs 2 to 20 characters (letters, digits, _-.[]).");
        w::SetFocus("nick");
        return false;
    }
    g_formError.clear();
    Cfg().nickname = nick;
    g_nick = Widen(nick);
    return true;
}

uint16_t PortValue() {
    int p = _wtoi(g_port.c_str());
    if (p <= 0 || p > 65535) p = 7788;
    return (uint16_t)p;
}

// the keys of the keys page, in its order
int* const* BoundKeys() {
    static int* const keys[] = {&Cfg().chatKey, &Cfg().passengerKey, &Cfg().playerListKey, &Cfg().voiceKey};
    return keys;
}
constexpr int kBoundKeys = 4;

// a letter or a digit as on the key cap whatever the layout ("Е" for T in a Russian one), the rest by Windows' names
std::string KeyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    switch (vk) {  // the extended ones, else Windows names the numpad's
        case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
        case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU: sc |= 0x100;
    }
    wchar_t name[64];
    if (sc && GetKeyNameTextW((LONG)(sc << 16), name, 64) > 0) return Narrow(name);
    return Format("#%d", vk);
}

// a key in a box: true when clicked
bool KeyField(float x, float y, float w, float h, const std::string& text, bool waiting) {
    auto& g = Gfx();
    auto& st = In().State();
    bool hover = st.mouseX >= x && st.mouseX < x + w && st.mouseY >= y && st.mouseY < y + h;
    g.Rect(x, y, w, h, waiting ? 0xFF16202A : 0xFF0C1117);
    g.RectOutline(x, y, w, h, waiting || hover ? kSaBlue : kSaLine, std::max(1.0f, g.Scale()));
    g.Text(FONT_NORMAL, x + w * 0.5f, y + (h - g.LineHeight(FONT_NORMAL)) * 0.5f, text, waiting ? kSaBlue : 0xFFFFFFFF,
           TEXT_CENTER | TEXT_SHADOW);
    return hover && st.lmbPressed;
}
}  // namespace

bool ConnectMenuKey(int vk) {
    if (!g_visible || g_capture < 0) return false;
    if (vk == VK_ESCAPE) return g_capture = -1, true;
    // F9 opens this window, Alt and Windows' keys belong to Windows
    if (vk == VK_F9 || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN) return true;
    int* const* keys = BoundKeys();
    for (int i = 0; i < kBoundKeys; ++i)
        if (i != g_capture && *keys[i] == vk) *keys[i] = *keys[g_capture];  // the action that had it takes the old one
    *keys[g_capture] = vk;
    g_capture = -1;
    flow::SaveConnectSettings();
    return true;
}

void SetConnectMenuVisible(bool visible) {
    if (visible == g_visible) return;
    g_visible = visible;
    g_keysPage = false;
    g_capture = -1;
    if (visible) {
        LoadFields();
        if (TheChat().IsOpen()) TheChat().Close();
        In().SetKeyboardCapture(true);
        In().SetMouseCapture(true);
    } else {
        In().SetKeyboardCapture(false);
        In().SetMouseCapture(false);
    }
}

bool IsConnectMenuVisible() { return g_visible; }

void ProcessConnectMenu() {
    // Show the window once on the first main-menu frame so the player sees how to start co-op.
    if (!g_autoShown && game::GameState() == game::GAME_STATE_FRONTEND_IDLE && !Cfg().autoConnect) {
        g_autoShown = true;
        SetConnectMenuVisible(true);
    }
    if (g_visible && NowMs() - g_slotsTime > 3000) RefreshSlots();
}

void RenderConnectMenu() {
    if (!g_visible) return;
    auto& g = Gfx();
    float s = g.Scale();
    auto& net = Net();
    bool online = net.Online();
    auto phase = flow::GetPhase();
    bool busy = phase == flow::Phase::StartingServer || phase == flow::Phase::Connecting ||
                phase == flow::Phase::WaitingWorld || phase == flow::Phase::LoadingWorld;
    bool inGame = core::IsPlayerReady();

    float lh = g.LineHeight(FONT_NORMAL), sh = g.LineHeight(FONT_SMALL), mh = g.LineHeight(FONT_MENU),
          ch = g.LineHeight(FONT_MENU_SMALL), bh = g.LineHeight(FONT_BIG);
    float rowH = lh + 14 * s;
    float W = 820 * s, H = 690 * s;
    float x = (g.Width() - W) * 0.5f, y = (g.Height() - H) * 0.5f;
    float cx = x + 52 * s, cw = W - 104 * s;
    float room = mh * 1.6f;  // the space beside a menu item's name

    float by = y + H - 20 * s - rowH;     // footer items
    float bodyBottom = by - 12 * s;

    g.Rect(0, 0, g.Width(), g.Height(), 0x99000000);
    w::BeginFrame();
    float cy = w::Panel(x, y, W, H, "SA:COOP") + 8 * s;
    float vx = x + W - 52 * s;
    g.Text(FONT_MENU_SMALL, vx, y + 36 * s, "v" COOP_VERSION_STR, kSaSlate, TEXT_RIGHT);
    // the language switch left of the version, the active one lit; the choice is saved at once
    float lx = vx - g.TextWidth(FONT_MENU_SMALL, "v" COOP_VERSION_STR) - 34 * s;
    for (int en = 1; en >= 0; --en) {
        const char* label = en ? "EN" : "RU";
        float tw = g.TextWidth(FONT_MENU_SMALL, label);
        lx -= tw;
        if (w::Tab(en ? "lang1" : "lang0", lx - 6 * s, y + 32 * s, tw + 12 * s, ch + 8 * s, label, Cfg().english == (en != 0), true)) {
            Cfg().english = en != 0;
            flow::SaveConnectSettings();
            RefreshSlots();
        }
        lx -= 52 * s;  // the space before EN
    }

    auto caption = [&](float px, const std::string& text) { g.Text(FONT_MENU_SMALL, px, cy, text, kSaMuted); };
    // wrapped paragraph, advances cy
    auto para = [&](const std::string& text, uint32_t color, FontId f = FONT_SMALL, bool centre = false) {
        for (const std::wstring& line : g.Wrap(f, Widen(text), cw)) {
            g.TextW(f, centre ? cx + cw * 0.5f : cx, cy, line, color, TEXT_SHADOW | (centre ? TEXT_CENTER : 0));
            cy += g.LineHeight(f);
        }
    };
    // a menu item whose text starts at px
    auto item = [&](const char* id, float px, float py, const std::string& label, bool enabled = true, bool lit = false) {
        return w::Button(id, px - room, py, g.TextWidth(FONT_MENU, label) + 2 * room, rowH, label, enabled, lit);
    };

    if (g_keysPage) {
        // ---- the mod's keys and the microphone, saved in coop.ini at once
        g.Text(FONT_BIG, cx, cy, TR("Клавиши", "Keys"), kSaBlue, TEXT_SHADOW);
        cy += bh + 22 * s;
        const char* labels[kBoundKeys] = {TR("Чат", "Chat"), TR("Сесть пассажиром", "Enter as a passenger"),
                                          TR("Список игроков", "Player list"), TR("Микрофон", "Microphone")};
        float kx = cx + cw * 0.55f;
        for (int i = 0; i < kBoundKeys; ++i) {
            g.Text(FONT_NORMAL, cx, cy + (rowH - lh) * 0.5f, labels[i], kText, TEXT_SHADOW);
            bool waiting = g_capture == i;
            if (KeyField(kx, cy, cx + cw - kx, rowH, waiting ? TR("нажмите клавишу", "press a key") : KeyName(*BoundKeys()[i]), waiting))
                g_capture = waiting ? -1 : i;
            cy += rowH + 10 * s;
        }
        cy += 14 * s;
        caption(cx, TR("Голосовая связь", "Voice chat"));
        cy += ch + 6 * s;
        auto& cfg = Cfg();
        int mode = !cfg.voiceEnabled ? 3 : cfg.voiceAlwaysOn ? 2 : cfg.voiceToggle ? 1 : 0;
        const char* modes[] = {TR("пока клавиша зажата", "while the key is held"),
                               TR("нажатие включает и выключает", "a press turns it on and off"), TR("всегда включена", "always on"),
                               TR("выключена", "off")};
        if (int d = w::Selector("voice", cx, cy, cw, rowH, modes[mode])) {
            mode = (mode + d + 4) % 4;
            cfg.voiceEnabled = mode != 3;
            cfg.voiceAlwaysOn = mode == 2;
            cfg.voiceToggle = mode == 1;
            flow::SaveConnectSettings();
        }
        cy += rowH;
    } else if (online) {
        // ---- in a session: who we are, the code for the friend, the players
        bool host = net.IsHost();
        g.Text(FONT_BIG, cx, cy, host ? TR("Сервер запущен", "Server running") : TR("Вы в игре", "Connected"), kSaBlue,
               TEXT_SHADOW);
        cy += bh + 2 * s;
        if (!host) para(Format(TR("Сервер «%s»", "Server \"%s\""), net.ServerName().c_str()), kSaMuted, FONT_NORMAL);
        cy += 22 * s;
        if (host) {
            caption(cx, TR("Код для друга", "Code for your friend"));
            cy += ch + 6 * s;
            // our wide capitals, not the game's pricedown: translations swap letters in that font
            // (a Russian build drew 2QMY as "2qmy")
            const std::string& code = net.InviteCode();
            if (code.empty()) {
                g.Text(FONT_NORMAL, cx, cy, TR("Узнаём внешний адрес...", "Finding out the external address..."), kSaMuted, TEXT_SHADOW);
                cy += bh;
            } else {
                g.Text(FONT_BIG, cx, cy, code, kGold, TEXT_OUTLINE);
                cy += bh + 2 * s;
                bool copied = NowMs() - g_copiedMs < 1500;
                if (item("copy", cx, cy, copied ? TR("Скопировано", "Copied") : TR("Скопировать", "Copy"), true, copied))
                    CopyToClipboard(code);
                cy += rowH;
            }
            cy += 20 * s;
        }
        caption(cx, host ? Format(TR("Игроки: %d из %d", "Players: %d of %d"), 1 + (int)net.Peers().size(), Cfg().maxPlayers)
                         : Format(TR("Игроки: %d", "Players: %d"), 1 + (int)net.Peers().size()));
        cy += ch + 8 * s;
        auto row = [&](const std::string& name, bool isHost, uint32_t ping, bool me) {
            float rh = lh + 14 * s;
            g.Rect(cx, cy, cw, rh, 0xFF090C10);
            g.Text(FONT_NORMAL, cx + 14 * s, cy + 7 * s, me ? name + TR(" (вы)", " (you)") : name, me ? 0xFFFFFFFF : kText,
                   TEXT_SHADOW);
            g.Text(FONT_MENU_SMALL, cx + cw * 0.55f, cy + (rh - ch) * 0.5f, isHost ? TR("хост", "host") : TR("напарник", "partner"),
                   isHost ? kSaBlue : kSaGreen);
            g.Text(FONT_SMALL, cx + cw - 14 * s, cy + (rh - sh) * 0.5f, Format(TR("%u мс", "%u ms"), ping), kSaMuted, TEXT_RIGHT);
            cy += rh + 4 * s;
        };
        row(Cfg().nickname, host, net.Rtt(), true);
        for (auto& [id, p] : net.Peers()) row(p.name, p.isHost, p.ping, false);
        if (host && net.Peers().empty()) para(TR("Ждём друга...", "Waiting for your friend..."), kSaMuted);
        // the colour of our own mark on the other players' radar (also /blip in the chat), saved and sent at once
        if (Cfg().partnerBlip) {
            static const char* const kColours[] = {"", "white", "red", "green", "yellow"};
            const char* names[] = {TR("обычный (синий)", "default (blue)"), TR("белый", "white"), TR("красный", "red"),
                                   TR("зелёный", "green"), TR("жёлтый", "yellow")};
            int i = 0;
            while (i < 5 && Cfg().markColour != kColours[i]) ++i;  // 5: a colour of one's own (RRGGBB)
            cy += 18 * s;
            caption(cx, TR("Цвет вашей метки у напарника", "Your mark's colour for your partner"));
            cy += ch + 6 * s;
            if (int d = w::Selector("blip", cx, cy, 360 * s, rowH, i < 5 ? names[i] : Cfg().markColour))
                Players::Instance().SetMarkColour(kColours[((i < 5 ? i : 0) + d + 5) % 5]);
            cy += rowH;
        }
    } else if (busy) {
        // ---- connecting / starting / loading, in the middle of the body
        float barH = 6 * s;
        cy += std::max(0.0f, (bodyBottom - cy - (mh * 2 + 18 * s + barH + 16 * s + sh)) * 0.5f);
        para(flow::StatusText(), kSaBlue, FONT_MENU, true);
        cy += 18 * s;
        g.Rect(cx, cy, cw, barH, 0xFF141A21);
        float p = phase == flow::Phase::WaitingWorld ? net.WorldProgress() : -1.0f;
        if (p >= 0) {
            g.Rect(cx, cy, cw * std::clamp(p, 0.0f, 1.0f), barH, kSaBlue);
        } else {  // nothing to measure: a block sliding back and forth
            float t = (float)(NowMs() % 1600) / 800.0f;
            float bw = cw * 0.25f;
            g.Rect(cx + (cw - bw) * (t < 1 ? t : 2 - t), cy, bw, barH, kSaBlue);
        }
        cy += barH + 16 * s;
        nat::Endpoint ep;
        uint8_t key = 0;
        if (phase == flow::Phase::Connecting && nat::ParseInviteCode(Trim(Narrow(g_addr)), ep, key))
            para(TR("Подключение по коду может занять до 20 секунд.", "Connecting by code can take up to 20 seconds."), kSaMuted,
                 FONT_SMALL, true);
        if (phase == flow::Phase::WaitingWorld && p < 0)
            para(TR("Хост сохраняет мир. Скоро он загрузится у вас.", "The host is saving the world. It will load for you soon."),
                 kSaMuted, FONT_SMALL, true);
    } else {
        // ---- the form: the tabs are menu items, the active one lit
        float tx = cx;
        for (int t = 0; t < 2; ++t) {
            const char* label = t ? TR("Создать игру", "Host a game") : TR("Подключиться", "Join a game");
            float tw = g.TextWidth(FONT_MENU, label);
            if (w::Tab(t ? "tab1" : "tab0", tx - room, cy, tw + 2 * room, rowH, label, g_tab == t)) {
                g_tab = t;
                g_formError.clear();
            }
            tx += tw + room * 2.2f;
        }
        cy += rowH + 4 * s;
        g.Rect(cx, cy, cw, std::max(1.0f, s), kSaLine);
        cy += 16 * s;

        caption(cx, TR("Ник", "Nickname"));
        cy += ch + 6 * s;
        bool enter = w::TextBox("nick", cx, cy, 320 * s, rowH, g_nick, 20);
        cy += rowH + 14 * s;

        int hostSlot = 0;
        if (g_tab == 0) {
            caption(cx, TR("Код приглашения или IP-адрес", "Invite code or IP address"));
            cy += ch + 6 * s;
            const char* paste = TR("Вставить", "Paste");
            float pw = g.TextWidth(FONT_MENU, paste) + room * 1.9f;
            enter |= w::TextBox("addr", cx, cy, cw - pw, rowH, g_addr, 64, false, false,
                                TR("например, 2QMY-7KAP-JR8Y", "for example, 2QMY-7KAP-JR8Y"));
            if (item("paste", cx + cw - g.TextWidth(FONT_MENU, paste), cy, paste)) {
                std::wstring clip = Widen(Trim(Narrow(w::ClipboardText())));
                if (!clip.empty()) g_addr = clip.substr(0, 64);
            }
        } else {
            caption(cx, TR("Сохранение", "Save"));
            cy += ch + 6 * s;
            if (inGame) {
                g.Rect(cx, cy, cw, rowH, 0xFF090C10);
                g.Text(FONT_NORMAL, cx + 12 * s, cy + (rowH - lh) * 0.5f, TR("Текущая игра", "Current game"), kText, TEXT_SHADOW);
            } else {
                int d = w::Selector("slot", cx, cy, cw, rowH, g_slots[g_slotIndex].title);
                if (d) g_slotIndex = (g_slotIndex + d + (int)g_slots.size()) % (int)g_slots.size();
                if (g_slots[g_slotIndex].exists) hostSlot = g_slots[g_slotIndex].slot;
            }
        }
        cy += rowH + 14 * s;

        caption(cx, TR("Порт", "Port"));
        caption(cx + 170 * s, TR("Пароль", "Password"));
        float playersW = g_tab == 1 ? 170 * s : 0.0f;  // beside the password, for the host: the window keeps its height
        if (g_tab == 1) caption(cx + cw - playersW, TR("Игроков", "Players"));
        cy += ch + 6 * s;
        enter |= w::TextBox("port", cx, cy, 150 * s, rowH, g_port, 5, false, true);
        enter |= w::TextBox("pass", cx + 170 * s, cy, cw - 170 * s - (g_tab == 1 ? playersW + 20 * s : 0.0f), rowH, g_password, 32, true, false,
                            TR("необязательно", "optional"));
        if (g_tab == 1) {
            int& players = Cfg().maxPlayers;
            if (int d = w::Selector("players", cx + cw - playersW, cy, playersW, rowH, std::to_string(players)))
                players = 2 + (players - 2 + d + proto::kMaxPlayers - 1) % (proto::kMaxPlayers - 1);
        }
        cy += rowH + 22 * s;
        if (g_tab == 1) {  // the host decides for the whole game; saved with the other settings when the server starts
            w::Checkbox("ff", cx, cy, TR("Урон по напарнику", "Friendly fire"), Cfg().friendlyFire);
            w::Checkbox("life", cx + cw * 0.5f, cy, TR("Общая жизнь", "Shared life"), Cfg().sharedLife);
            cy += lh * 0.8f + 22 * s;
            bool noTeleport = !Cfg().partnerTeleport;
            if (w::Checkbox("tp", cx, cy, TR("Запретить напарникам /tp и /tphere", "No /tp and /tphere for partners"), noTeleport))
                Cfg().partnerTeleport = !noTeleport;
            cy += lh * 0.8f + 22 * s;
        }

        // the main action: a big menu item in the middle, lit like the selected entry of the game's menus
        bool can = g_tab == 0 ? !Trim(Narrow(g_addr)).empty() : inGame || hostSlot != 0;
        if ((w::Button("go", cx, cy, cw, bh + 10 * s, g_tab == 0 ? TR("Подключиться", "Connect") : TR("Создать сервер", "Start the server"),
                       can, true, true) ||
             (enter && can)) &&
            ApplyNick()) {
            if (g_tab == 0) flow::Join(Trim(Narrow(g_addr)), PortValue(), Narrow(g_password));
            else flow::Host(inGame ? 0 : hostSlot, PortValue(), Narrow(g_password));
        }
        cy += bh + 10 * s + 14 * s;

        std::string error = !g_formError.empty() ? g_formError : flow::LastError();
        if (!error.empty()) para(error, kSaRed, FONT_NORMAL, true);
        else if (g_tab == 0 && inGame)
            para(TR("Текущая игра закроется. Вместо неё загрузится мир друга.",
                    "Your current game will close, and your friend's world will load instead."),
                 kSaMuted, FONT_SMALL, true);
        else if (g_tab == 1 && !inGame && !hostSlot)
            para(TR("В этом слоте нет сохранения.", "This slot has no save."), kSaMuted, FONT_SMALL, true);
        else if (g_tab == 1 && !inGame && hostSlot < 0)
            para(TR("Пролог вы пройдёте сами, а друг появится рядом, когда CJ сядет на велосипед.",
                    "You play the prologue alone, and your friend appears next to you when CJ gets on the bike."),
                 kSaMuted, FONT_SMALL, true);
    }

    // ---- footer: like "Back" on the game's pages
    g.Rect(cx, bodyBottom, cw, std::max(1.0f, s), kSaLine);
    float fx = cx;
    if (online || busy) {
        const char* leave = online ? TR("Отключиться", "Disconnect") : TR("Отмена", "Cancel");
        if (item("leave", fx, by, leave)) flow::Leave();
        fx += g.TextWidth(FONT_MENU, leave) + room * 2.2f;
    }
    if (item("keys", fx, by, g_keysPage ? TR("Назад", "Back") : TR("Клавиши", "Keys"))) g_keysPage = !g_keysPage, g_capture = -1;
    std::string close = online || busy || inGame ? TR("Закрыть", "Close") : TR("Одиночная игра", "Single player");
    if (item("close", cx + cw - g.TextWidth(FONT_MENU, close), by, close)) SetConnectMenuVisible(false);
    w::EndFrame();
    w::DrawCursor();
    // Esc closes even when a text box is focused (handled in OnKeyDown)
}

}  // namespace coop::ui

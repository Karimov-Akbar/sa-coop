// Emotes (Emotes.h): the list, our CJ playing one, pairs with the other player, the players' own animation files.
//
// Our CJ plays an emote by the script command a mission plays the player's animations with (TASK_PLAY_ANIM; the upper
// body only: TASK_PLAY_ANIM_SECONDARY). The game's events end it as they end a mission's animation of the player: a hit,
// a fall, a car, a cutscene. The other games see it as any script animation of ours (Anims.cpp): the copy plays it with
// its own tasks kept off it. Never the non-interruptable command: CJ would stand on through bullets and cars.
//
// It does not start (WhyNot) in a vehicle, in the air or the water, crouched, in a cutscene or a scene of the game's
// camera, while a script holds the controls (shops, the phone, minigames, contests), while CJ is busy with a task of his
// own (falling, getting up, climbing, getting into a car, a jetpack, carrying something, dying) or rides a toy or a gun on
// a car. Once on, a pose, a dance or a sit lasts until the player presses Space or F, a one-shot until it ends or the
// player moves (each with its own "out" animation when it has one), and it is left to the game when a script or a
// cutscene takes CJ over. CJ puts his weapon away for it and takes it back after.
//
// A pair: the one who starts it asks, the other answers with Y or N. The one who says yes is put in front of the asker,
// facing it, as far as the game's own scenes put the two (a metre: CJ and Sweet, the gang's greetings, the dates), and
// both start their part together. Either one moving or getting hurt stops both.
#include "features/anims/Emotes.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "features/anims/Ifp.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Anim.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"

namespace coop::anims {

namespace {
using namespace game;
using game::script::Cmd;

constexpr int TASK_SIMPLE_NAMED_ANIM = 401, TASK_SIMPLE_HOLD_ENTITY = 307, TASK_SIMPLE_SWIM = 1304,
              TASK_COMPLEX_USE_MOBILE_PHONE = 1600;
enum : uint8_t { OP_ASK = 1, OP_NO = 2, OP_GO = 3, OP_STOP = 4 };
constexpr uint64_t kAnswerMs = 20000;
constexpr float kPairRange = 4.0f;  // the two players stand this close to start a pair

// The players' own animations take the end of the game's animation table (CAnimManager::ms_aAnimations, 2500 places):
// the game gives each of its blocks its places from the start as the block first loads (1871 in all, our COOPCS 36, a
// cutscene's 18 at most), and a block loaded after ours took places after them (the table ran past its end).
constexpr int kTailStart = 2150, kTailEnd = 2500;
constexpr size_t kMaxFiles = 32, kMaxFileBytes = 4u << 20, kMaxAllBytes = 16u << 20;

struct Custom {
    std::string file;   // shown in the list
    std::string block;  // ours: "C_" (a player's own) or "P_" (a pack of the mod's) and the file's name
    int index = -1;     // the block here
    std::vector<std::string> anims;
};
std::vector<Custom> g_custom, g_packs;
bool g_customTried = false, g_customBroken = false;
int g_tailUsed = 0;

std::vector<Emote> g_list;
bool g_listEnglish = false;
size_t g_listCustom = (size_t)-1;

// a pack's rows: the animations of its file that are in it, under its title (the names in the list point into the
// loaded pack's, which stay as long as the game runs)
struct PackRow {
    uint16_t id;
    std::string anim;
    std::string name;  // TR at the call: the list is built again when the language changes
    uint8_t flags;
    const char* outBlock = nullptr;  // how CJ gets up from it: the game's own animation (none: he straightens up)
    const char* out = nullptr;
};
void AddPack(const char* file, const std::string& title, const char* author, const std::vector<PackRow>& rows) {
    const Custom* p = nullptr;
    for (const Custom& c : g_packs)
        if (!_stricmp(c.file.c_str(), file)) p = &c;
    if (!p) return;
    for (const PackRow& r : rows)
        for (const std::string& a : p->anims)
            if (!_stricmp(a.c_str(), r.anim.c_str()))
                g_list.push_back({r.id, PACKS, r.name, p->block.c_str(), nullptr, a.c_str(), r.out, r.flags, nullptr, 0.0f,
                                  title + " · " + author, r.outBlock});
}

// numbered rows: anim prefix01, 02... as "name 1", "name 2"..., the ones in `moving` going where they take CJ
std::vector<PackRow> Numbered(uint16_t first, const char* prefix, int count, const char* name, std::initializer_list<int> moving,
                              uint8_t flags) {
    std::vector<PackRow> rows;
    for (int i = 1; i <= count; ++i) {
        bool moves = std::find(moving.begin(), moving.end(), i) != moving.end();
        rows.push_back({(uint16_t)(first + i - 1), Format("%s%02d", prefix, i), Format("%s %d", name, i),
                        (uint8_t)(flags | (moves ? MOVES : 0))});
    }
    return rows;
}

// gestures, poses, idle actions, dances, tricks; the flags as each animation goes (its length, how far its root
// moves: MOVES past half a metre)
void AddPacks() {
    AddPack("salute", TR("Отдать честь", "Salute"), "KOT33", {{601, "Salute", TR("Отдать честь", "Salute"), HOLD}});
    AddPack("gtavsalute", TR("Приветствие из GTA V", "Greeting from GTA V"), "Alexander (Skone Modding)",
            {{602, "Salute", TR("Отдать честь как в GTA V", "Salute as in GTA V"), HOLD}});
    AddPack("rapping", TR("Рэп", "Rap"), "Aleksey_Move", {{603, "Rap", TR("Читать рэп", "Rap"), LOOP | MOVES}});
    AddPack("idlepose", TR("Позы", "Poses"), "Androsz, mrandres5555",
            {{611, "Pose01", TR("Руки в боки", "Hands on hips"), LOOP},
             {612, "Pose02", TR("Руки на груди", "Hands on the chest"), LOOP},
             {613, "Pose03", TR("Поднятая рука", "A hand raised"), LOOP},
             {614, "Pose04", TR("Стоять ровно", "Stand straight"), LOOP},
             {615, "Pose05", TR("Смотреть вдаль", "Look into the distance"), LOOP},
             {616, "Pose06", TR("Руки в стороны", "Arms spread"), LOOP},
             {617, "Pose07", TR("Модельная поза", "Model pose"), LOOP},
             {618, "Pose08", TR("На одном колене", "On one knee"), LOOP},
             {619, "Pose09", TR("Оглянуться", "Look aside"), LOOP}});
    // Off the ground with the game's own (their roots start as low as these poses' 0.85-1.0 m): on the back as from the
    // beach (Lay_Bac_out), on the side or curled up as after a fall (getup), sitting as from the park's grass. A pose ended
    // with none straightened CJ up from the ground in a quarter of a second.
    AddPack("restpose", TR("Позы на земле", "Poses on the ground"), "Ruslan Konorov",
            {{621, "Rest01", TR("Задуматься", "Think it over"), LOOP},
             {622, "Rest02", TR("Лечь на бок", "Lie on the side"), LOOP, "PED", "getup"},
             {623, "Rest03", TR("Лечь на спину", "Lie on the back"), LOOP, "SUNBATHE", "Lay_Bac_out"},
             {624, "Rest04", TR("Сидеть с вытянутыми ногами", "Sit with the legs stretched"), LOOP, "SUNBATHE", "ParkSit_M_out"},
             {625, "Rest05", TR("Свернуться", "Curl up"), LOOP, "PED", "getup"},
             {626, "Rest06", TR("Сидеть по-турецки", "Sit cross-legged"), LOOP, "SUNBATHE", "ParkSit_M_out"},
             {627, "Rest07", TR("Отдыхать лёжа", "Lie back"), LOOP, "SUNBATHE", "Lay_Bac_out"}});
    AddPack("lcsidle", TR("Действия из GTA LCS", "Idle actions from GTA LCS"), "Mystertv, Junior_Djjr",
            {{631, "LcsShift", TR("Переминаться", "Shift the weight"), 0},
             {632, "LcsStretch", TR("Потянуться", "Stretch"), 0},
             {633, "LcsTime", TR("Посмотреть на часы", "Check the time"), 0},
             {634, "LcsLegs", TR("Размять ноги", "Stretch the legs"), 0},
             {635, "LcsShoulders", TR("Размять плечи", "Roll the shoulders"), 0}});
    AddPack("grafdance", TR("Диско-танцы", "Disco dances"), "GRAF MC",
            Numbered(641, "Graf", 16, TR("Танец", "Dance"), {11, 12, 13, 14, 15, 16}, LOOP));
    AddPack("fortnite", TR("Танцы из Fortnite", "Fortnite dances"), "Sergio Persson",
            Numbered(661, "Fortnite", 16, TR("Танец", "Dance"), {}, LOOP));
    AddPack("housedance", TR("Танцы с вечеринки", "Party dances"), "ATField02_GR",
            {{683, "House03", TR("Танец 1", "Dance 1"), LOOP},
             {684, "House04", TR("Танец 2", "Dance 2"), LOOP | MOVES},
             {685, "House05", TR("Танец 3", "Dance 3"), LOOP},
             {686, "House06", TR("Танец 4", "Dance 4"), LOOP}});
    AddPack("newmoves", TR("Брейк и трюки", "Breakdance and tricks"), "Arch",
            {{691, "Move01", TR("Три арабских сальто", "Three arabians"), MOVES},
             {692, "Move02", TR("Арабское сальто", "Arabian"), MOVES},
             {693, "Move03", TR("Балет", "Ballet"), MOVES},
             {694, "Move04", TR("Брейк 1", "Break 1"), MOVES},
             {695, "Move05", TR("Брейк 2", "Break 2"), MOVES},
             {696, "Move06", TR("Удар ногой 1", "Kick 1"), MOVES},
             {697, "Move07", TR("Удар ногой 2", "Kick 2"), MOVES},
             {698, "Move08", TR("Удар ногой 3", "Kick 3"), MOVES},
             {699, "Move09", TR("Колесо", "Cartwheel"), MOVES},
             {700, "Move10", TR("Вращение на голове", "Headspin"), MOVES},
             {701, "Move11", TR("Прыжки на руках", "Hand hops"), MOVES}});
    AddPack("acrostreet", TR("Акробатика", "Acrobatics"), "Spirit",
            {{711, "Acro01", TR("Два фляка и подъём разгибом", "Two backflips to kip-up"), MOVES},
             {712, "Acro02", TR("Штопор 720", "Corkscrew 720"), MOVES},
             {713, "Acro03", TR("Штопор 720 и 360", "Cork 720 to cork 360"), MOVES},
             {714, "Acro04", TR("Чит-гейнер", "Cheat gainer"), MOVES},
             {715, "Acro05", TR("Подъём разгибом", "Kip-up"), MOVES},
             {716, "Acro06", TR("Подъём разгибом и сальто вперёд", "Kip-up to front flip"), MOVES},
             {717, "Acro07", TR("Фляк на одной руке и флэш", "One-hand backspring to flash"), MOVES},
             {718, "Acro08", TR("Рондат и сальто назад", "Round-off backflip"), MOVES},
             {719, "Acro09", TR("Рондат и двойное сальто назад", "Round-off double backflip"), MOVES},
             {720, "Acro10", TR("Рондат и сальто прогнувшись", "Round-off layout"), MOVES},
             {721, "Acro11", TR("Флэш", "Flash"), MOVES},
             {722, "Acro12", TR("Флэш и два штопора 720", "Flash to 720 to 720"), MOVES}});
    AddPack("parkour", TR("Паркур из MTA", "Parkour from MTA"), "Paul_Cortez",
            {{731, "BckHndSpingBTuck", TR("Сальто назад", "Back tuck"), MOVES},
             {732, "BckHndSping", TR("Фляк назад", "Back handspring"), MOVES},
             {733, "CartWheel", TR("Колесо", "Cartwheel"), MOVES},
             {734, "FrntHndSpring", TR("Переворот вперёд", "Front handspring"), MOVES},
             {735, "HandPlant", TR("Опора на руку", "Hand plant"), MOVES}});
}

void BuildList() {
    g_list.clear();
    // The list keeps to what SA-MP's role play servers use, Samp-Rp's /anim going by its sections (the user's word, 04.11: all
    // 1678 of the game had too much that shows nothing good), and leaves out what CJ does in play by himself: blows and
    // fight stances, aiming and reloading, throws, sneaking, the idle stretches. Advance RP's 88 rows were read off a video of
    // its window (04.11): it names no animations, so its rows are matched by their motion in the game's files, the first 27
    // by SA-MP's gl_actions, whose order they keep. Lying and falling get up with the game's own:
    // straightened at once CJ rose from the ground in a quarter of a second.
    auto add = [](uint16_t id, Kind k, const char* name, const char* block, const char* in, const char* main, const char* out,
                  uint8_t flags, const char* other = nullptr, float apart = 0.0f, const char* outBlock = nullptr) {
        g_list.push_back({id, k, name, block, in, main, out, flags, other, apart, std::string(), outBlock});
    };
    auto onGround = [&add](uint16_t id, const char* name, const char* block, const char* main, uint8_t flags) {
        add(id, POSE, name, block, nullptr, main, "getup", flags, nullptr, 0.0f, "ped");
    };
    // emotions: gestures, faces of the body
    add(1, EMOTION, TR("Помахать рукой", "Wave"), "ON_LOOKERS", "wave_in", "wave_loop", "wave_out", LOOP);
    add(2, EMOTION, TR("Показать пальцем", "Point"), "ON_LOOKERS", "point_in", "point_loop", "point_out", LOOP);
    add(3, EMOTION, TR("Показать наверх", "Point up"), "ON_LOOKERS", "Pointup_in", "Pointup_loop", "Pointup_out", LOOP);
    add(4, EMOTION, TR("Звать", "Call out"), "ON_LOOKERS", "shout_in", "shout_loop", "shout_out", LOOP);
    add(5, EMOTION, TR("Крикнуть", "Shout"), "ON_LOOKERS", nullptr, "shout_01", nullptr, 0);
    add(6, EMOTION, TR("Смеяться", "Laugh"), "RAPPING", nullptr, "Laugh_01", nullptr, 0);
    add(7, EMOTION, TR("Злиться", "Angry"), "RIOT", nullptr, "RIOT_ANGRY", nullptr, 0);
    add(8, EMOTION, TR("Объяснять жестами", "Explain with the hands"), "RIOT", nullptr, "RIOT_challenge", nullptr, 0);
    add(9, EMOTION, TR("Скандировать", "Chant"), "RIOT", nullptr, "RIOT_CHANT", nullptr, LOOP);
    add(10, EMOTION, TR("Средний палец", "Middle finger"), "ped", nullptr, "fucku", nullptr, PART);
    add(11, EMOTION, TR("Фейспалм", "Facepalm"), "MISC", nullptr, "plyr_shkhead", nullptr, PART);
    add(12, EMOTION, TR("Подозвать", "Beckon"), "RYDER", nullptr, "RYD_Beckon_01", nullptr, PART);
    add(13, EMOTION, TR("Болеть", "Cheer"), "OTB", nullptr, "wtchrace_cmon", nullptr, 0);
    add(14, EMOTION, TR("Радоваться", "Celebrate"), "OTB", nullptr, "wtchrace_win", nullptr, 0);
    add(15, EMOTION, TR("Расстроиться", "Upset"), "OTB", nullptr, "wtchrace_lose", nullptr, 0);
    add(16, EMOTION, TR("Испугаться", "Scared"), "ped", nullptr, "handscower", nullptr, 0);
    add(17, EMOTION, TR("Паника", "Panic"), "ON_LOOKERS", "panic_in", "panic_loop", "panic_out", LOOP);
    add(18, EMOTION, TR("Руки вверх", "Hands up"), "SHOP", nullptr, "SHP_HandsUp_Scr", nullptr, HOLD);
    add(21, EMOTION, TR("Оглядеться", "Look around"), "ON_LOOKERS", "lkaround_in", "lkaround_loop", "lkaround_out", LOOP);
    add(22, EMOTION, TR("Смотреть вверх", "Look up"), "ON_LOOKERS", "lkup_in", "lkup_loop", "lkup_out", LOOP);
    add(24, EMOTION, TR("Согласиться", "Agree"), "GANGS", nullptr, "Invite_Yes", nullptr, 0);
    add(25, EMOTION, TR("Отказаться", "Refuse"), "GANGS", nullptr, "Invite_No", nullptr, 0);
    add(27, EMOTION, TR("Разговаривать", "Talk"), "GANGS", nullptr, "prtial_gngtlkA", nullptr, PART);
    add(28, EMOTION, TR("Думать", "Think"), "COP_AMBIENT", "Coplook_in", "Coplook_think", "Coplook_out", LOOP);
    add(29, EMOTION, TR("Кивать", "Nod"), "COP_AMBIENT", "Coplook_in", "Coplook_nod", "Coplook_out", LOOP);
    add(31, EMOTION, TR("Знак банды 1", "Gang sign 1"), "GHANDS", nullptr, "gsign1", nullptr, PART);
    add(32, EMOTION, TR("Знак банды 2", "Gang sign 2"), "GHANDS", nullptr, "gsign2", nullptr, PART);
    add(33, EMOTION, TR("Знак банды 3", "Gang sign 3"), "GHANDS", nullptr, "gsign3", nullptr, PART);
    add(34, EMOTION, TR("Знак банды 4", "Gang sign 4"), "GHANDS", nullptr, "gsign4", nullptr, PART);
    add(35, EMOTION, TR("Знак банды 5", "Gang sign 5"), "GHANDS", nullptr, "gsign5", nullptr, PART);
    add(36, EMOTION, TR("Разговаривать 2", "Talk 2"), "GANGS", nullptr, "prtial_gngtlkB", nullptr, PART);
    add(37, EMOTION, TR("Разговаривать 3", "Talk 3"), "GANGS", nullptr, "prtial_gngtlkC", nullptr, PART);
    add(38, EMOTION, TR("Разговаривать 4", "Talk 4"), "GANGS", nullptr, "prtial_gngtlkD", nullptr, PART);
    add(39, EMOTION, TR("Болтать", "Chat"), "ped", nullptr, "IDLE_chat", nullptr, LOOP);
    add(40, EMOTION, TR("Кричать", "Yell"), "RIOT", nullptr, "RIOT_shout", nullptr, 0);
    add(41, EMOTION, TR("Средний палец двумя руками", "Both middle fingers"), "RIOT", nullptr, "RIOT_FUKU", nullptr, 0);
    add(43, EMOTION, TR("Шлёпнуть", "Slap on the bottom"), "SWEET", nullptr, "sweet_ass_slap", nullptr, 0);
    add(45, EMOTION, TR("Почесаться", "Scratch"), "MISC", nullptr, "Scratchballs_01", nullptr, 0);
    add(46, EMOTION, TR("Сжаться от страха", "Cower"), "ped", nullptr, "cower", nullptr, LOOP);
    add(47, EMOTION, TR("Скорбеть", "Mourn"), "GRAVEYARD", nullptr, "mrnM_loop", nullptr, LOOP);
    add(48, EMOTION, TR("Скорбеть 2", "Mourn 2"), "GRAVEYARD", nullptr, "mrnF_loop", nullptr, LOOP);
    add(49, EMOTION, TR("Не соглашаться", "Disagree"), "COP_AMBIENT", "Coplook_in", "Coplook_shake", "Coplook_out", LOOP);
    add(50, EMOTION, TR("Рассматривать", "Examine"), "COP_AMBIENT", "Copbrowse_in", "Copbrowse_loop", "Copbrowse_out", LOOP);
    add(51, EMOTION, TR("Крикнуть 2", "Shout 2"), "ON_LOOKERS", nullptr, "shout_02", nullptr, 0);
    add(52, EMOTION, TR("Помахать 2", "Wave 2"), "KISSING", nullptr, "gfwave2", nullptr, 0);
    add(53, EMOTION, TR("Помахать 3", "Wave 3"), "BD_FIRE", nullptr, "BD_GF_Wave", nullptr, 0);
    add(54, EMOTION, TR("Команда: вперёд", "Signal: go"), "SWAT", nullptr, "swt_go", nullptr, 0);
    add(55, EMOTION, TR("Команда: смотри", "Signal: look"), "SWAT", nullptr, "swt_lkt", nullptr, 0);
    add(56, EMOTION, TR("Команда: стоять", "Signal: stay"), "SWAT", nullptr, "swt_sty", nullptr, 0);
    add(57, EMOTION, TR("Посмотреть на часы", "Check the watch"), "CLOTHES", nullptr, "CLO_Pose_Watch", nullptr, 0);
    add(58, EMOTION, TR("Осмотреть обувь", "Look at the shoes"), "CLOTHES", nullptr, "CLO_Pose_Shoes", nullptr, 0);
    add(59, EMOTION, TR("Осмотреть одежду", "Look at the clothes"), "CLOTHES", nullptr, "CLO_Pose_Torso", nullptr, 0);
    add(60, EMOTION, TR("Вытянуть руку перед собой", "Arm out in front"), "SHOP", nullptr, "ROB_Loop_Threat", nullptr, LOOP);
    add(61, EMOTION, TR("Закрыть лицо от ужаса", "Cover the face in horror"), "ON_LOOKERS", nullptr, "panic_hide", nullptr, LOOP);
    add(63, EMOTION, TR("Попрощаться", "Say goodbye"), "ped", nullptr, "endchat_01", nullptr, PART);
    add(64, EMOTION, TR("Наклониться, чтобы расслышать", "Lean in to hear"), "BAR", nullptr, "Barserve_order", nullptr, 0);
    add(65, EMOTION, TR("Подглядывать", "Peek"), "SWAT", nullptr, "swt_wllpk_L", nullptr, LOOP);
    add(66, EMOTION, TR("Размахивать руками", "Wave the arms about"), "BD_FIRE", nullptr, "BD_Panic_03", nullptr, LOOP);
    // poses: the body holds them until CJ moves
    add(101, POSE, TR("Прислониться", "Lean"), "GANGS", "leanIN", "leanIDLE", "leanOUT", LOOP);
    add(102, POSE, TR("Сесть на землю", "Sit on the ground"), "SUNBATHE", "ParkSit_M_in", "ParkSit_M_IdleA", "ParkSit_M_out", LOOP);
    add(103, POSE, TR("Лечь на спину", "Lie on the back"), "SUNBATHE", nullptr, "Lay_Bac_in", "Lay_Bac_out", HOLD);
    add(104, POSE, TR("Скрестить руки", "Arms crossed"), "COP_AMBIENT", "Coplook_in", "Coplook_loop", "Coplook_out", LOOP);
    add(105, POSE, TR("Автостоп", "Hitchhike"), "MISC", nullptr, "Hiker_Pose", nullptr, HOLD);
    add(106, POSE, TR("Охранник", "Guard"), "DEALER", nullptr, "DEALER_IDLE", nullptr, LOOP);
    add(110, POSE, TR("Стоять с сигаретой", "Smoke standing"), "SMOKING", nullptr, "M_smkstnd_loop", nullptr, LOOP);
    add(111, POSE, TR("Курить у стены", "Smoke by a wall"), "SMOKING", nullptr, "M_smklean_loop", nullptr, LOOP);
    add(112, POSE, TR("Скрестить руки 2", "Arms crossed 2"), "GRAVEYARD", nullptr, "prst_loopa", nullptr, LOOP);
    add(113, POSE, TR("Прислониться к стене", "Lean on a wall"), "MISC", nullptr, "Plyrlean_loop", nullptr, LOOP);
    add(114, POSE, TR("Сесть на стул", "Sit on a chair"), "ped", "SEAT_down", "SEAT_idle", "SEAT_up", LOOP);
    add(115, POSE, TR("Сесть на ступеньку", "Sit on a step"), "Attractors", "Stepsit_in", "Stepsit_loop", "Stepsit_out", LOOP);
    add(116, POSE, TR("Сесть, закинув ногу на ногу", "Sit with the legs crossed"), "INT_HOUSE", "LOU_In", "LOU_Loop", "LOU_Out", LOOP);
    add(117, POSE, TR("Сидеть за столом", "Sit at a desk"), "INT_OFFICE", "OFF_Sit_In", "OFF_Sit_Idle_Loop", "OFF_Sit_2Idle_180", LOOP);
    add(118, POSE, TR("Скучать за столом", "Bored at a desk"), "INT_OFFICE", "OFF_Sit_In", "OFF_Sit_Bored_Loop", "OFF_Sit_2Idle_180", LOOP);
    add(119, POSE, TR("Печатать за столом", "Type at a desk"), "INT_OFFICE", "OFF_Sit_In", "OFF_Sit_Type_Loop", "OFF_Sit_2Idle_180", LOOP);
    add(120, POSE, TR("Сидеть в кафе", "Sit in a cafe"), "FOOD", "FF_Sit_In", "FF_Sit_Loop", "FF_Sit_Out_180", LOOP);
    add(121, POSE, TR("Сидеть на траве", "Sit on the grass"), "SUNBATHE", "ParkSit_W_in", "ParkSit_W_idleA", "ParkSit_W_out", LOOP);
    add(122, POSE, TR("Сидеть и ждать", "Sit and wait"), "BEACH", nullptr, "SitnWait_loop_W", "SitnWait_out_W", LOOP, nullptr, 0.0f, "SUNBATHE");
    add(123, POSE, TR("Загорать", "Sunbathe"), "BEACH", nullptr, "bather", "batherup", LOOP, nullptr, 0.0f, "SUNBATHE");
    add(124, POSE, TR("Лечь, опираясь на руку", "Lie propped on a hand"), "INT_HOUSE", "BED_In_L", "BED_Loop_L", "BED_Out_L", LOOP);
    onGround(125, TR("Ранен", "Wounded"), "SWEET", "Sweet_injuredloop", LOOP);
    onGround(126, TR("Полулежать", "Recline"), "CRACK", "crckidle1", LOOP);
    onGround(127, TR("Спать на боку", "Sleep on the side"), "CRACK", "crckidle2", LOOP);
    onGround(128, TR("Сидеть, вытянув ноги", "Sit with the legs out"), "CRACK", "crckidle3", LOOP);
    onGround(129, TR("Спать на спине", "Sleep on the back"), "CRACK", "crckidle4", LOOP);
    onGround(130, TR("Эффект электрошокера", "Taser shock"), "CRACK", "crckdeth2", LOOP);
    onGround(131, TR("Упасть замертво", "Drop dead"), "CRACK", "crckdeth4", HOLD);
    add(132, POSE, TR("Упасть лицом вниз", "Fall face down"), "ped", nullptr, "KO_skid_front", "getup_front", HOLD);
    add(133, POSE, TR("Упасть на спину", "Fall on the back"), "ped", nullptr, "KO_shot_stom", "getup", HOLD);
    add(141, POSE, TR("Ловить такси", "Hail a taxi"), "ped", nullptr, "IDLE_taxi", nullptr, HOLD);
    add(142, POSE, TR("Стойка охранника", "Bouncer stance"), "ped", nullptr, "IDLE_HBHB", nullptr, LOOP);
    add(143, POSE, TR("Поднять руки", "Raise the hands"), "SHOP", nullptr, "SHP_Rob_HandsUp", nullptr, HOLD);
    add(144, POSE, TR("Пить стоя", "Drink standing"), "BAR", nullptr, "dnk_stndM_loop", nullptr, LOOP);
    onGround(145, TR("Кашлять и упасть", "Cough and drop"), "ped", "gas_cwr", HOLD);
    add(146, POSE, TR("Присесть и закрыть голову", "Crouch and cover the head"), "ped", nullptr, "DUCK_cower", nullptr, LOOP);
    add(147, POSE, TR("Стоять у стойки", "Stand at the bar"), "BAR", nullptr, "Barcustom_loop", nullptr, LOOP);
    add(148, POSE, TR("Поза вратаря", "Goalkeeper stance"), "BSKTBALL", nullptr, "BBALL_def_loop", nullptr, LOOP);
    add(149, POSE, TR("Говорить сидя", "Talk seated"), "MISC", nullptr, "Seat_talk_01", "SEAT_up", LOOP, nullptr, 0.0f, "ped");
    add(150, POSE, TR("Говорить сидя 2", "Talk seated 2"), "MISC", nullptr, "Seat_talk_02", "SEAT_up", LOOP, nullptr, 0.0f, "ped");
    add(151, POSE, TR("Женское курение", "Smoke, woman's way"), "SMOKING", nullptr, "F_smklean_loop", nullptr, LOOP);
    // actions
    add(201, ACTION, TR("Курить", "Smoke"), "SMOKING", "M_smk_in", "M_smk_loop", "M_smk_out", LOOP);
    add(202, ACTION, TR("Читать рэп", "Rap"), "RAPPING", "RAP_A_IN", "RAP_A_Loop", "RAP_A_OUT", LOOP);
    add(203, ACTION, TR("Читать рэп 2", "Rap 2"), "RAPPING", "RAP_B_IN", "RAP_B_Loop", "RAP_B_OUT", LOOP);
    add(204, ACTION, TR("Читать рэп 3", "Rap 3"), "RAPPING", nullptr, "RAP_C_Loop", nullptr, LOOP);
    add(206, ACTION, TR("Тай-чи", "Tai chi"), "PARK", "Tai_Chi_in", "Tai_Chi_Loop", "Tai_Chi_Out", LOOP);
    add(207, ACTION, TR("Регулировать движение", "Direct traffic"), "POLICE", nullptr, "CopTraf_Come", nullptr, LOOP);
    add(209, ACTION, TR("Делать массаж сердца", "CPR"), "MEDIC", nullptr, "CPR", nullptr, 0);
    add(210, ACTION, TR("Осмотреть землю", "Check the ground"), "WUZI", nullptr, "Wuzi_grnd_chk", nullptr, 0);
    add(211, ACTION, TR("Сфотографировать", "Take a photo"), "CAMERA", "picstnd_in", "picstnd_take", "picstnd_out", LOOP);
    add(215, ACTION, TR("Мыть руки", "Wash hands"), "INT_HOUSE", nullptr, "wash_up", nullptr, 0);
    add(217, ACTION, TR("Стучать в окно", "Bang on a window"), "MISC", nullptr, "bng_wndw", nullptr, 0);
    add(218, ACTION, TR("Говорить по телефону", "Talk on the phone"), "ped", "phone_in", "phone_talk", "phone_out", LOOP);
    add(219, ACTION, TR("Есть", "Eat"), "FOOD", nullptr, "EAT_Burger", nullptr, 0);
    add(222, ACTION, TR("Стошнить", "Vomit"), "FOOD", nullptr, "EAT_Vomit_P", nullptr, 0);
    add(223, ACTION, TR("Пить из банки", "Drink from a can"), "VENDING", nullptr, "VEND_Drink_P", nullptr, 0);
    add(224, ACTION, TR("Есть из кармана", "Eat from the pocket"), "VENDING", nullptr, "VEND_Eat_P", nullptr, 0);
    add(225, ACTION, TR("Пить пиво", "Drink a beer"), "GANGS", nullptr, "drnkbr_prtl", nullptr, PART);
    add(226, ACTION, TR("Курить на ходу", "Smoke on the move"), "GANGS", nullptr, "smkcig_prtl", nullptr, PART);
    add(232, ACTION, TR("Отдать деньги", "Hand over the cash"), "SHOP", nullptr, "SHP_Rob_GiveCash", nullptr, 0);
    add(233, ACTION, TR("Заплатить", "Pay"), "DEALER", nullptr, "shop_pay", nullptr, 0);
    add(235, ACTION, TR("Обыск", "Get searched"), "POLICE", nullptr, "crm_drgbst_01", nullptr, 0);
    add(237, ACTION, TR("Регулировщик: стоп", "Traffic: stop"), "POLICE", nullptr, "CopTraf_Stop", nullptr, LOOP);
    add(238, ACTION, TR("Регулировщик: налево", "Traffic: left"), "POLICE", nullptr, "CopTraf_Left", nullptr, LOOP);
    add(239, ACTION, TR("Регулировщик: проезжайте", "Traffic: go on"), "POLICE", nullptr, "CopTraf_Away", nullptr, LOOP);
    add(240, ACTION, TR("Толкать машину", "Push a car"), "GANGS", nullptr, "shake_cara", nullptr, LOOP);
    add(252, ACTION, TR("Справить нужду", "Take a leak"), "PAULNMAC", "Piss_in", "Piss_loop", "Piss_out", LOOP);
    add(253, ACTION, TR("Работать за стойкой", "Tend the bar"), "BAR", nullptr, "Barserve_loop", nullptr, LOOP);
    add(255, ACTION, TR("Пьяная походка", "Walk drunk"), "ped", nullptr, "WALK_drunk", nullptr, LOOP | MOVES);
    add(257, ACTION, TR("Сделать заказ", "Place an order"), "BAR", nullptr, "Barcustom_order", nullptr, 0);
    add(258, ACTION, TR("Выбирать товар", "Look over the goods"), "INT_SHOP", "shop_in", "shop_loop", "shop_out", LOOP);
    add(259, ACTION, TR("Взять с полки", "Take from a shelf"), "INT_SHOP", nullptr, "shop_shelf", nullptr, 0);
    add(260, ACTION, TR("Чинить машину лёжа", "Fix a car lying down"), "CAR", nullptr, "Fixn_Car_Loop", "Fixn_Car_Out", LOOP);
    add(261, ACTION, TR("Говорить в окно машины", "Talk at a car window"), "ped", nullptr, "car_hookertalk", nullptr, LOOP);
    add(262, ACTION, TR("Банкомат", "Use a cash machine"), "ped", nullptr, "ATM", nullptr, MOVES);
    add(263, ACTION, TR("Снимать с колена", "Take a photo kneeling"), "CAMERA", "piccrch_in", "piccrch_take", "piccrch_out", LOOP);
    add(264, ACTION, TR("Подложить бомбу", "Plant a bomb"), "BOMBER", "BOM_Plant_In", "BOM_Plant_Loop", "BOM_Plant_2Idle", LOOP);
    add(265, ACTION, TR("Надеть маску", "Put on a mask"), "goggles", nullptr, "goggles_put_on", nullptr, PART);
    add(266, ACTION, TR("Взять что-то и рассмотреть", "Pick something up and look at it"), "INT_SHOP", nullptr, "shop_lookA", nullptr, 0);
    add(267, ACTION, TR("Толкнуть боком", "Shove with the shoulder"), "GANGS", nullptr, "shake_carSH", nullptr, 0);
    add(268, ACTION, TR("Предложить наркотики", "Offer drugs"), "DEALER", nullptr, "DEALER_DEAL", nullptr, 0);
    // dances
    add(301, DANCE, TR("Танец 1", "Dance 1"), "DANCING", nullptr, "dnce_M_a", nullptr, LOOP);
    add(302, DANCE, TR("Танец 2", "Dance 2"), "DANCING", nullptr, "dnce_M_b", nullptr, LOOP);
    add(303, DANCE, TR("Танец 3", "Dance 3"), "DANCING", nullptr, "dnce_M_c", nullptr, LOOP);
    add(304, DANCE, TR("Танец 4", "Dance 4"), "DANCING", nullptr, "dnce_M_d", nullptr, LOOP);
    add(305, DANCE, TR("Танец 5", "Dance 5"), "DANCING", nullptr, "dnce_M_e", nullptr, LOOP);
    add(306, DANCE, TR("Танец 6", "Dance 6"), "DANCING", nullptr, "dance_loop", nullptr, LOOP);
    add(307, DANCE, TR("Танец 7", "Dance 7"), "DANCING", nullptr, "DAN_Loop_A", nullptr, LOOP);
    add(308, DANCE, "G-Funk", "GFUNK", nullptr, "dance_loop", nullptr, LOOP);
    add(309, DANCE, "Running Man", "RUNNINGMAN", nullptr, "dance_loop", nullptr, LOOP);
    add(310, DANCE, "WOP", "WOP", nullptr, "dance_loop", nullptr, LOOP);
    add(311, DANCE, TR("Танец 8", "Dance 8"), "DANCING", nullptr, "DAN_Down_A", nullptr, LOOP);
    add(312, DANCE, TR("Танец 9", "Dance 9"), "DANCING", nullptr, "DAN_Left_A", nullptr, LOOP);
    add(313, DANCE, TR("Танец 10", "Dance 10"), "DANCING", nullptr, "DAN_Right_A", nullptr, LOOP);
    add(314, DANCE, TR("Танец 11", "Dance 11"), "DANCING", nullptr, "DAN_Up_A", nullptr, LOOP);
    add(315, DANCE, TR("Элемент восточного танца", "Eastern dance move"), "BSKTBALL", nullptr, "BBALL_react_score", nullptr, LOOP);
    // pairs: the asker plays main, the other one `other`; as far apart as the game's scripts put the two
    add(401, PAIR, TR("Рукопожатие", "Handshake"), "GANGS", nullptr, "hndshkfa", nullptr, 0, nullptr, 1.0f);
    add(402, PAIR, TR("Приветствие банды 1", "Gang greeting 1"), "GANGS", nullptr, "hndshkaa", nullptr, 0, nullptr, 1.0f);
    add(403, PAIR, TR("Приветствие банды 2", "Gang greeting 2"), "GANGS", nullptr, "hndshkba", nullptr, 0, nullptr, 1.0f);
    add(404, PAIR, TR("Приветствие банды 3", "Gang greeting 3"), "GANGS", nullptr, "hndshkca", nullptr, 0, "hndshkcb", 1.0f);
    add(405, PAIR, TR("Приветствие банды 4", "Gang greeting 4"), "GANGS", nullptr, "hndshkda", nullptr, 0, nullptr, 1.0f);
    add(406, PAIR, TR("Приветствие банды 5", "Gang greeting 5"), "GANGS", nullptr, "hndshkea", nullptr, 0, nullptr, 1.0f);
    add(407, PAIR, TR("Пожать руку", "Shake hands"), "GANGS", nullptr, "prtial_hndshk_01", nullptr, 0, nullptr, 1.0f);
    add(408, PAIR, TR("Деловое рукопожатие", "Business handshake"), "GANGS", nullptr, "prtial_hndshk_biz_01", nullptr, 0, nullptr,
        0.92f);
    add(409, PAIR, TR("Рука на плечо", "Hand on the shoulder"), "SWEET", nullptr, "plyr_hndshldr_01", nullptr, 0,
        "sweet_hndshldr_01", 0.9f);
    add(410, PAIR, TR("Сделка", "Deal"), "GANGS", nullptr, "DEALER_DEAL", nullptr, 0, "DRUGS_BUY", 1.0f);
    add(411, PAIR, TR("Подарок", "Gift"), "KISSING", nullptr, "gift_give", nullptr, 0, "gift_get", 1.0f);
    add(412, PAIR, TR("Спор", "Argue"), "PAULNMAC", nullptr, "PnM_Argue1_A", nullptr, 0, "PnM_Argue1_B", 1.2f);
    add(413, PAIR, TR("Спор 2", "Argue 2"), "PAULNMAC", nullptr, "PnM_Argue2_A", nullptr, 0, "PnM_Argue2_B", 1.2f);
    // a metre apart, as the kiss of "Burning Desire" puts the two
    add(414, PAIR, TR("Поцелуй", "Kiss"), "KISSING", nullptr, "Playa_Kiss_03", nullptr, 0, "Grlfrd_Kiss_03", 1.0f);
    add(415, PAIR, TR("Поцелуй 2", "Kiss 2"), "KISSING", nullptr, "Playa_Kiss_01", nullptr, 0, "Grlfrd_Kiss_01", 1.0f);
    add(416, PAIR, TR("Поцелуй 3", "Kiss 3"), "KISSING", nullptr, "Playa_Kiss_02", nullptr, 0, "Grlfrd_Kiss_02", 1.0f);
    if (g_customBroken) return;
    AddPacks();  // 601 on
    // the players' own files, numbered from 501 in the order they were read
    uint16_t id = 501;
    for (const Custom& c : g_custom)
            for (const std::string& a : c.anims)
                g_list.push_back({id++, CUSTOM, c.file + " · " + a, c.block.c_str(), nullptr, a.c_str(), nullptr, LOOP, nullptr, 0.0f});
}

void* FindTask(CPed* ped, int type) {
    void* intel = ped ? ped->Intelligence() : nullptr;
    return intel ? CallMethod<0x681740, void*>((uint8_t*)intel + 4, type) : nullptr;  // CTaskManager::FindActiveTaskByType
}

// our CJ's script animation of that name, primary or the upper body's
void* NamedTask(CPed* ped, const std::string& anim) {
    void* t = anim.empty() ? nullptr : FindTask(ped, TASK_SIMPLE_NAMED_ANIM);
    return t && !_stricmp((const char*)t + 0x10, anim.c_str()) ? t : nullptr;  // CTaskSimpleRunNamedAnim::m_animName
}

// the block in memory (asked for when it is not: false meanwhile)
bool BlockReady(const char* block) { return NeedBlock(Call<0x4D3990, int>(block)); }  // CAnimManager::GetAnimationBlockIndex

// an animation's key as the game's blocks keep it (CAnimBlendHierarchy::m_hashKey)
uint32_t AnimKey(const std::string& name) { return Call<0x53CF30, uint32_t>(name.c_str()); }  // CKeyGen::GetUppercaseKey

// a script or the game holds CJ: a shop, the phone, a scene, a minigame, a contest
bool Scripted() {
    return GetPad(0)->DisablePlayerControls || cam::WideScreenOn() || Ref<uint8_t>(0xA444A8) /* CTheScripts::bMiniGameInProgress */ ||
           ContestActivity() || SceneWatched();
}

bool CutsceneOn() { return Ref<uint8_t>(0xB5F851) || Ref<int32_t>(0xB5F84C); }  // CCutsceneMgr::ms_running, ms_cutsceneLoadStatus

struct Run {
    enum Stage : uint8_t { OFF, LOADING, INTRO, MAIN, OUTRO };
    Stage stage = OFF;
    uint16_t id = 0;
    std::string block, in, main, out, outBlock;  // outBlock: out's own block (empty: block)
    uint8_t flags = 0;
    uint64_t sinceMs = 0;    // the stage began
    bool calm = false;       // the pad was let go once since: a key held from before stops nothing
    float at = 0, total = 0; // where our animation was last seen (an end by itself is no stop for the other player)
    int weapon = 0;          // the weapon CJ put away for it
    uint8_t with = 0xFF;     // a pair: the other player
    uint64_t startMs = 0;    // a pair's answering side starts as the other side gets the word
};
Run g_run;

struct Ask {
    uint8_t who = 0xFF;
    uint16_t id = 0;
    uint64_t until = 0;
};
Ask g_asked;   // ours, until its answer
Ask g_asking;  // the other player's, until ours

void Send(uint8_t op, uint8_t to, uint16_t id) {
    Writer w(proto::G_EMOTE);
    w.U8(op);
    w.U8(to);
    w.U16(id);
    Net().Send(proto::CH_RELIABLE, w);
}

void GiveBackWeapon(CPlayerPed* me) {
    int type = std::exchange(g_run.weapon, 0);
    if (!type || !me || me->Health() <= 0.0f || me->InVehicle() || me->ActiveWeapon().type != 0) return;
    for (int i = 0; i < NUM_WEAPON_SLOTS; ++i)
        if (me->Weapons()[i].type == type) {
            Cmd(0x01B9).I(GetPedRef(me)).I(type).Run();  // SET_CURRENT_CHAR_WEAPON
            return;
        }
}

// over: the weapon back unless a script took CJ (its own scene sets what he holds); the other player of a pair stops too
// unless ours ran to its end
void End(bool giveBack, bool tellOther) {
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (giveBack) GiveBackWeapon(me);
    if (tellOther && g_run.with != 0xFF && Net().Peers().count(g_run.with)) Send(OP_STOP, g_run.with, g_run.id);
    LOGI("Emote %u ended%s", g_run.id, giveBack ? "" : " (taken over)");
    g_run = Run{};
}

void PlayStage(CPlayerPed* me, const std::string& anim, bool loop, bool hold, const std::string& block = g_run.block) {
    bool part = g_run.flags & PART, moves = g_run.flags & MOVES;
    // TASK_PLAY_ANIM / TASK_PLAY_ANIM_SECONDARY: ped, animation, block, blend, loop, lock X, lock Y, hold the last frame,
    // time (-1: as long as it goes). The locks move CJ with the animation's root: a flip played in place went metres off
    // and sprang back at its end
    Cmd(part ? 0x0A1A : 0x0605).I(GetPedRef(me)).S(anim.c_str()).S(block.c_str()).F(4.0f).I(loop).I(moves).I(moves).I(hold).I(-1).Run();
    g_run.sinceMs = NowMs();
    g_run.at = g_run.total = 0;
}

// A pose, a stance, a dance, a sit (looped or held, the whole body) lasts until the player ends it with Space or F
// (sprint, enter a car), as SA-MP's animations do (the user's choice, 03.11: "чтобы длились долго, как в самп"): WASD
// ended them at once. A one-shot still stops at any move, and a hit, a fall or a car ends either (the game's events).
bool Lasting() { return (g_run.flags & (LOOP | HOLD)) && !(g_run.flags & PART); }
bool PadEnds() {
    const CControllerState& s = GetPad(0)->NewState;
    return s.ButtonCross || s.ButtonTriangle;
}
bool g_toldEnds = false;  // the chat said how a lasting one ends, once a game

void StartStages(CPlayerPed* me) {
    if (!(g_run.flags & PART) && me->ActiveWeapon().type != 0) {
        g_run.weapon = me->ActiveWeapon().type;
        Cmd(0x01B9).I(GetPedRef(me)).I(0).Run();  // SET_CURRENT_CHAR_WEAPON: put away, a gun in a dancing hand floats
    }
    if (Lasting() && !std::exchange(g_toldEnds, true))
        ui::TheChat().AddSystem(TR("Пробел или F останавливают анимацию.", "Space or F stops the animation."));
    g_run.calm = false;
    if (!g_run.in.empty()) {
        g_run.stage = Run::INTRO;
        PlayStage(me, g_run.in, false, true);  // held on its last frame: no gap before the loop
    } else {
        g_run.stage = Run::MAIN;
        PlayStage(me, g_run.main, g_run.flags & LOOP, g_run.flags & HOLD);
    }
    LOGI("Emote %u: %s/%s%s", g_run.id, g_run.block.c_str(), g_run.in.empty() ? g_run.main.c_str() : g_run.in.c_str(),
         g_run.with != 0xFF ? " (a pair)" : "");
}

void Start(const Emote& e, const char* mine, uint8_t with, uint64_t startMs) {
    int weapon = g_run.stage != Run::OFF ? g_run.weapon : 0;  // one emote after another: the weapon put away for the first
    g_run = Run{};
    g_run.stage = Run::LOADING;
    g_run.id = e.id;
    g_run.block = e.block;
    g_run.in = e.in ? e.in : "";
    g_run.main = mine;
    g_run.out = e.out ? e.out : "";
    g_run.outBlock = e.outBlock ? e.outBlock : "";
    g_run.flags = e.flags;
    g_run.weapon = weapon;
    g_run.with = with;
    g_run.startMs = startMs;
    g_run.sinceMs = NowMs();
}

// any button of moving or acting on foot
bool PadMoves() {
    const CControllerState& s = GetPad(0)->NewState;
    return std::abs(s.LeftStickX) > 40 || std::abs(s.LeftStickY) > 40 || s.ButtonSquare || s.ButtonCross || s.ButtonTriangle ||
           s.ButtonCircle || s.RightShoulder1 || s.LeftShoulder1 || s.ShockButtonL;
}

// the player ends it: its "out" animation, else the animation blends out (CLEAR_CHAR_TASKS). An out of another block is
// played only while that block is in memory (TASK_PLAY_ANIM of a block not loaded finds no animation).
void StopByPlayer(CPlayerPed* me) {
    const std::string& outBlock = g_run.outBlock.empty() ? g_run.block : g_run.outBlock;
    if (g_run.stage == Run::MAIN && !g_run.out.empty() && BlockReady(outBlock.c_str()) &&
        anim::Find(Call<0x4D3990, int>(outBlock.c_str()), AnimKey(g_run.out))) {
        g_run.stage = Run::OUTRO;
        PlayStage(me, g_run.out, false, false, outBlock);
        if (g_run.with != 0xFF && Net().Peers().count(g_run.with)) Send(OP_STOP, g_run.with, g_run.id);
        g_run.with = 0xFF;
        return;
    }
    Cmd(0x0687).I(GetPedRef(me)).Run();  // CLEAR_CHAR_TASKS
    End(true, true);
}

void Update() {
    if (g_run.stage == Run::OFF) return;
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (!me) return void(g_run = Run{});
    if (CutsceneOn() || Scripted() || me->InVehicle()) return End(false, true);  // the game's now: its own animations on CJ
    bool input = Lasting() ? PadEnds() : PadMoves();
    bool react = g_run.calm && input && NowMs() - g_run.sinceMs > 300;
    if (!input) g_run.calm = true;
    switch (g_run.stage) {
        case Run::LOADING: {
            bool outReady = g_run.outBlock.empty() || BlockReady(g_run.outBlock.c_str());  // asked for with the rest
            if (!BlockReady(g_run.block.c_str()) || !outReady) {
                if (NowMs() - g_run.sinceMs > 6000) {
                    ui::TheChat().AddError(TR("Анимация не загрузилась.", "The animation did not load."));
                    End(true, true);
                }
                return;
            }
            // the animation in this game's block: a mod's IFP in place of the game's may lack it, and the task would find none
            int bi = Call<0x4D3990, int>(g_run.block.c_str());
            auto lacks = [bi](const std::string& name) { return !name.empty() && !anim::Find(bi, AnimKey(name)); };
            if (lacks(g_run.main) || lacks(g_run.in)) {
                ui::TheChat().AddError(TR("Этой анимации нет в вашей игре.", "This animation is not in your game."));
                return End(true, true);
            }
            if (NowMs() < g_run.startMs) return;
            // still free for it (a pair's other side said go): CJ just put in front of the other one stands on the ground a
            // frame or two later, so a while is given
            if (std::string why = WhyNot(); !why.empty()) {
                if (NowMs() - g_run.sinceMs > 3000) {
                    ui::TheChat().AddError(why);
                    End(true, true);
                }
                return;
            }
            return StartStages(me);
        }
        case Run::INTRO: {
            void* t = NamedTask(me, g_run.in);
            if (!t && NowMs() - g_run.sinceMs < 400) return;  // the script command's task comes with CJ's next events
            if (!t) return End(true, true);                   // a hit, a fall: the game's
            if (react) return StopByPlayer(me);
            void* a = Field<void*>(t, 0x08);  // CTaskSimpleAnim::m_pAnim
            if (a && anim::CurrentTime(a) >= anim::TotalTime(anim::Hierarchy(a)) - 0.02f) {
                g_run.stage = Run::MAIN;
                PlayStage(me, g_run.main, g_run.flags & LOOP, g_run.flags & HOLD);
            }
            return;
        }
        case Run::MAIN: {
            void* t = NamedTask(me, g_run.main);
            if (!t && g_run.total == 0.0f && NowMs() - g_run.sinceMs < 400) return;  // not on yet
            if (!t) {
                // over by itself (a one-shot that ran to its end) or ended by the game: the other player of a pair stops
                // only for the latter
                bool done = g_run.total > 0 && g_run.at >= g_run.total - 0.15f;
                return End(true, !done && NowMs() - g_run.sinceMs > 200);
            }
            if (void* a = Field<void*>(t, 0x08)) g_run.at = anim::CurrentTime(a), g_run.total = anim::TotalTime(anim::Hierarchy(a));
            // a gesture of the upper body goes on while CJ walks; the rest stop at the player's word (Lasting)
            if (react && !(g_run.flags & PART)) StopByPlayer(me);
            return;
        }
        case Run::OUTRO:
            if (!NamedTask(me, g_run.out) && NowMs() - g_run.sinceMs > 200) End(true, false);
            return;
        default:
            return;
    }
}

// The blocks this feature asked the streaming for (not in memory then), and when each was last needed. Leafing through
// the game's animations in the window kept up to 11 MB of IFPs (all of anim.img) for good: RequestModel's GAME_REQUIRED
// is taken off again (CStreaming::SetModelIsDeletable; a script's MISSION_REQUIRED stays) once nothing plays from the
// block (CAnimBlock::RefCnt), neither our CJ's animation nor the preview uses it (its copy plays without a reference) and
// 3 s have gone since it was needed. The streaming drops it when it needs the room.
std::map<int, uint64_t> g_ourBlocks;

void ReleaseBlocks() {
    static uint64_t checkedMs = 0;
    if (g_ourBlocks.empty() || NowMs() - checkedMs < 1000) return;
    checkedMs = NowMs();
    int run[2] = {-1, -1};
    if (g_run.stage != Run::OFF) {
        run[0] = Call<0x4D3990, int>(g_run.block.c_str());
        if (!g_run.outBlock.empty()) run[1] = Call<0x4D3990, int>(g_run.outBlock.c_str());
    }
    for (auto it = g_ourBlocks.begin(); it != g_ourBlocks.end();) {
        int i = it->first;
        bool used = i == run[0] || i == run[1] || NowMs() - it->second < 3000 || PreviewHolds(i) ||
                    (anim::BlockLoaded(i) && Ref<int16_t>(anim::Block(i) + 0x12) > 0);  // CAnimBlock::RefCnt
        if (used) {
            ++it;
            continue;
        }
        Call<0x409C10>(anim::kIfpBase + i);  // CStreaming::SetModelIsDeletable
        it = g_ourBlocks.erase(it);
    }
}

const RemotePlayer* Partner(CPlayerPed* me) {
    const RemotePlayer* best = nullptr;
    float bestD = kPairRange;
    for (auto& [id, rp] : Players::Instance().All()) {
        if (!rp.ped || !rp.hasState || rp.mode != RemotePlayer::Mode::OnFoot || rp.ped->InVehicle() || rp.state.area != me->AreaCode())
            continue;
        CVector d = rp.state.pos - me->GetPosition();
        if (d.Length2D() < bestD && std::fabs(d.z) < 1.5f) best = &rp, bestD = d.Length2D();
    }
    return best;
}

// the place in front of that player, facing it, on ground as high as its own and seen from it (no wall, car or thing
// between): false when there is none
bool Spot(const RemotePlayer& rp, float apart, CVector& out, float& headingDeg) {
    const CVector& a = rp.state.pos;  // its ped: a metre over its feet
    float h = rp.state.rotation;
    CVector at{a.x - std::sin(h) * apart, a.y + std::cos(h) * apart, a.z};
    bool found = false;
    float ground = Call<0x5696C0, float>(at.x, at.y, a.z + 0.5f, &found, (CEntity**)nullptr);  // CWorld::FindGroundZFor3DCoord
    if (!found || std::fabs(ground - (a.z - 1.0f)) > 0.6f) return false;
    CVector from{a.x, a.y, a.z + 0.4f}, to{at.x, at.y, ground + 1.4f};
    if (!Call<0x56A490, bool>(&from, &to, true, true, false, true, false, false, false)) return false;  // CWorld::GetIsLineOfSightClear
    out = {at.x, at.y, ground};
    headingDeg = h * 57.29578f + 180.0f;
    return true;
}

// ---- the players' own files

// "C_" (or a pack's "P_") and the file's name in capital Latin letters, digits and _, as much as a block's name takes
// (15): the same for the other player whatever case its file's name is in
std::string BlockName(const std::wstring& file, const char* prefix) {
    std::string s = prefix;
    for (wchar_t c : file.substr(0, file.find_last_of(L'.'))) {
        if (s.size() >= 15) break;
        if (c >= L'a' && c <= L'z') s += (char)(c - 32);
        else if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L'_') s += (char)c;
    }
    return s.size() > 2 ? s : "";
}

std::string Shown(const std::string& s) {  // a name from a file, as text the interface can draw
    std::string out;
    for (char c : s) out += c >= 0x20 && c < 0x7F ? c : '?';
    return out;
}

void LoadOne(const std::wstring& file, std::vector<uint8_t>& bytes, const char* what, const char* prefix, std::vector<Custom>& into) {
    std::string name = WideToUtf8(file);
    IfpInfo info;
    if (!CheckIfp(bytes.data(), bytes.size(), info)) return LOGW("%s: %s is not an animation file of San Andreas", what, name.c_str());
    std::string block = BlockName(file, prefix);
    if (block.empty()) return LOGW("%s: %s: the file's name needs Latin letters or digits", what, name.c_str());
    if (Call<0x4D3990, int>(block.c_str()) >= 0) return LOGW("%s: %s: the name %s is taken", what, name.c_str(), block.c_str());
    int n = (int)info.anims.size();
    if (kTailStart + g_tailUsed + n > kTailEnd || Ref<int32_t>(0xB4EA30) + 3 >= 180)  // CAnimManager::ms_numAnimBlocks
        return LOGW("%s: no room left for %s (%d animations)", what, name.c_str(), n);
    // our block with its places at the end of the table, given before the load: the loader keeps a known block's places
    // (0x4D520A) and only moves the table's count on, set back after it
    int index = Call<0x4D3E50, int>(block.c_str());  // CAnimManager::RegisterAnimBlock
    uintptr_t b = anim::Block(index);
    Ref<int32_t>(b + 0x14) = kTailStart + g_tailUsed;  // FirstAnimIdx
    Ref<int32_t>(b + 0x18) = n;                        // NumAnims
    memset(bytes.data() + 8, 0, 24);
    memcpy(bytes.data() + 8, block.c_str(), block.size());
    int count = Ref<int32_t>(0xB4EA2C);  // CAnimManager::ms_numAnimations
    struct RwMemory {
        uint8_t* start;
        uint32_t length;
    } mem{bytes.data(), (uint32_t)bytes.size()};
    static const char kNoneUncompressed[1][32] = {""};
    if (void* stream = Call<0x7ECEF0, void*>(3, 1, (void*)&mem)) {  // RwStreamOpen(rwSTREAMMEMORY, rwSTREAMREAD)
        Call<0x4D47F0>(stream, false, kNoneUncompressed);           // CAnimManager::LoadAnimFile
        Call<0x7ECE20, int>(stream, (void*)&mem);                   // RwStreamClose
    }
    Ref<int32_t>(0xB4EA2C) = count;
    if (!anim::BlockLoaded(index)) return LOGW("%s: the game did not take %s", what, name.c_str());
    Custom c;
    c.file = Shown(name.substr(0, name.find_last_of('.')));
    c.block = block;
    c.index = index;
    for (auto& a : info.anims) c.anims.push_back(Shown(a));
    g_tailUsed += n;
    LOGI("%s: %s as block %s (%d animations)", what, name.c_str(), block.c_str(), n);
    into.push_back(std::move(c));
}

void LoadFolder(const wchar_t* folder, const char* what, const char* prefix, std::vector<Custom>& into) {
    std::wstring dir = Cfg().coopDir + folder;
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.ifp").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.nFileSizeHigh == 0 && fd.nFileSizeLow <= kMaxFileBytes)
            files.push_back(fd.cFileName);
    while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(files.begin(), files.end());
    if (files.size() > kMaxFiles) LOGW("%s: %zu files, the first %zu are read", what, files.size(), kMaxFiles), files.resize(kMaxFiles);
    size_t all = 0;
    for (const std::wstring& f : files) {
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(dir + f, bytes) || (all += bytes.size()) > kMaxAllBytes) continue;
        LoadOne(f, bytes, what, prefix, into);
    }
}

// the mod's packs first: the players' own files after them take what is left of the places
void LoadCustom() {
    LoadFolder(L"packs\\", "Packs", "P_", g_packs);
    LoadFolder(L"anims\\", "Own animations", "C_", g_custom);
}

class Emotes : public Module {
public:
    const char* Name() const override { return "Emotes"; }
    void OnInstallHooks() override {
        Net().SetHandler(proto::G_EMOTE, [](uint8_t from, Reader& r) { Handle(from, r); });
    }
    void OnPostProcess() override {
        // the players' own files, once, before any cutscene loads its block: it removes the last block when it ends
        // (CAnimManager::RemoveLastAnimFile), which must be its own
        if (!g_customTried && core::IsPlayerReady() && !CutsceneOn() && !Ref<uint8_t>(0xB5F850)) {  // CCutsceneMgr::ms_animLoaded
            g_customTried = true;
            LoadCustom();
        }
        // the game's blocks reached our places at the end of the table (a game with many animations of mods): ours are
        // no longer whole, left unused
        if (!g_customBroken && (!g_custom.empty() || !g_packs.empty()) && Ref<int32_t>(0xB4EA2C) > kTailStart) {
            g_customBroken = true;
            LOGW("Own animations and packs: the game's animation table reached %d, its own animations go over ours: ours are "
                 "off", Ref<int32_t>(0xB4EA2C));
        }
        Update();
        ReleaseBlocks();
        if (g_asked.who != 0xFF && NowMs() >= g_asked.until) {
            ui::TheChat().AddSystem(Format(TR("Нет ответа от игрока %s.", "No answer from %s."), Net().PeerName(g_asked.who).c_str()));
            g_asked = {};
        }
    }
    void OnGameStarted() override { g_run = Run{}, g_asked = g_asking = {}; }
    void OnSessionEnd() override {
        g_asked = g_asking = {};
        g_run.with = 0xFF;
    }
    void OnPeerLeave(uint8_t id) override {
        if (g_asked.who == id) g_asked = {};
        if (g_asking.who == id) g_asking = {};
        if (g_run.with == id) g_run.with = 0xFF;
    }

    static void Handle(uint8_t from, Reader& r) {
        uint8_t op = r.U8(), to = r.U8();
        uint16_t id = r.U16();
        if (!r.Ok() || to != Net().MyId() || !Net().Peers().count(from)) return;
        std::string name = Net().PeerName(from);
        const Emote* e = Find(id);
        if (op == OP_ASK) {
            if (!e || e->kind != PAIR) return;
            if (g_asked.who == from) g_asked = {};  // both asked at once: ours gives way
            g_asking = {from, id, NowMs() + kAnswerMs};
            BlockReady(e->block);  // in memory by the answer
            ui::TheChat().AddSystem(Format(TR("%s предлагает анимацию вдвоём: %s.", "%s offers a pair animation: %s."), name.c_str(),
                                           e->name.c_str()));
            LOGI("Pair emote %u offered by %s", id, name.c_str());
        } else if (op == OP_NO && g_asked.who == from) {
            g_asked = {};
            ui::TheChat().AddSystem(Format(TR("%s отказывается.", "%s says no."), name.c_str()));
        } else if (op == OP_GO && g_asked.who == from && g_asked.id == id && e) {
            g_asked = {};
            if (std::string why = WhyNot(); !why.empty()) {
                Send(OP_STOP, from, id);
                ui::TheChat().AddError(why);
                return;
            }
            Start(*e, e->main, from, 0);
            LOGI("Pair emote %u with %s: go", id, name.c_str());
        } else if (op == OP_STOP && g_run.with == from) {
            g_run.with = 0xFF;  // stopped there: not told back
            CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
            if (!me) return;
            if (g_run.stage == Run::LOADING) return End(true, false);
            if (g_run.stage == Run::INTRO || g_run.stage == Run::MAIN) StopByPlayer(me);
        }
    }
};
Emotes g_module;
struct Reg {
    Reg() { core::RegisterModule(&g_module); }
} g_reg;
}  // namespace

const char* KindName(Kind k) {
    switch (k) {
        case EMOTION: return TR("Эмоции", "Emotions");
        case POSE: return TR("Позы", "Poses");
        case ACTION: return TR("Действия", "Actions");
        case DANCE: return TR("Танцы", "Dances");
        case PAIR: return TR("Вдвоём", "Together");
        case PACKS: return TR("Паки", "Packs");
        default: return TR("Свои", "Own");
    }
}

const std::vector<Emote>& List() {
    size_t custom = g_customBroken ? 0 : g_custom.size() + (g_packs.size() << 16);
    if (g_list.empty() || g_listEnglish != Cfg().english || g_listCustom != custom) {
        BuildList();
        g_listEnglish = Cfg().english;
        g_listCustom = custom;
    }
    return g_list;
}

const Emote* Find(uint16_t id) {
    for (const Emote& e : List())
        if (e.id == id) return &e;
    return nullptr;
}

std::string WhyNot() {
    CPlayerPed* me = core::IsPlayerReady() ? FindPlayerPed(0) : nullptr;
    if (!me) return TR("Игра ещё не началась.", "The game has not started yet.");
    if (CutsceneOn()) return TR("Сейчас идёт катсцена.", "A cutscene is on.");
    if (Scripted()) return TR("Сейчас CJ управляет игра.", "The game controls CJ right now.");
    if (me->InVehicle()) return TR("В машине анимации не включаются.", "Animations do not work in a vehicle.");
    if (me->IsDucking()) return TR("Сначала встаньте.", "Stand up first.");
    if (!(me->PedFlags() & 1) || me->IsInTheAir() || FindTask(me, TASK_SIMPLE_SWIM))  // CPed::bIsStanding
        return TR("CJ должен стоять на земле.", "CJ must stand on the ground.");
    int task = ActivePrimaryTaskType(me);
    if ((task != 0 && task != TASK_SIMPLE_NAMED_ANIM) || me->Health() <= 0.0f || FindTask(me, TASK_COMPLEX_USE_MOBILE_PHONE) ||
        FindTask(me, TASK_SIMPLE_HOLD_ENTITY) || me->AttachedTo() || PlayerInfos()[0].remoteVehicle)
        return TR("CJ сейчас занят.", "CJ is busy right now.");
    return "";
}

bool Play(const Emote& e, std::string& why) {
    why = WhyNot();
    if (!why.empty()) return false;
    if ((e.kind == CUSTOM || e.kind == PACKS) && g_customBroken)
        return why = TR("Анимации из файлов отключены, подробности в логе.", "Animations from files are off, see the log."), false;
    if (e.kind != PAIR) {
        Start(e, e.main, 0xFF, 0);
        return true;
    }
    CPlayerPed* me = FindPlayerPed(0);
    const RemotePlayer* rp = Net().Online() ? Partner(me) : nullptr;
    if (!rp) return why = TR("Подойдите к напарнику ближе.", "Come closer to your partner."), false;
    if (g_asked.who != 0xFF) return why = TR("Приглашение уже отправлено. Ждём ответа.", "The invitation is sent. Waiting for the answer."), false;
    Send(OP_ASK, rp->id, e.id);
    g_asked = {rp->id, e.id, NowMs() + kAnswerMs};
    BlockReady(e.block);
    ui::TheChat().AddSystem(Format(TR("Приглашение отправлено игроку %s.", "Invitation sent to %s."), Net().PeerName(rp->id).c_str()));
    LOGI("Pair emote %u offered to %s", e.id, Net().PeerName(rp->id).c_str());
    return true;
}

bool Key(bool yes) {
    if (g_asking.who == 0xFF || NowMs() >= g_asking.until) return false;
    Ask a = std::exchange(g_asking, Ask{});
    if (!Net().Peers().count(a.who)) return true;
    if (!yes) {
        Send(OP_NO, a.who, a.id);
        return true;
    }
    std::string why = WhyNot();
    const Emote* e = Find(a.id);
    CPlayerPed* me = FindPlayerPed(0);
    const RemotePlayer* rp = Players::Instance().Get(a.who);
    CVector spot;
    float heading = 0;
    if (why.empty() && (!e || !rp || Partner(me) != rp)) why = TR("Подойдите к напарнику ближе.", "Come closer to your partner.");
    if (why.empty() && !Spot(*rp, e->apart, spot, heading))
        why = TR("Здесь тесно. Отойдите на свободное место.", "There is no room here. Move to an open spot.");
    if (!why.empty()) {
        Send(OP_NO, a.who, a.id);
        ui::TheChat().AddError(why);
        return true;
    }
    Cmd(0x00A1).I(GetPedRef(me)).F(spot.x).F(spot.y).F(spot.z).Run();  // SET_CHAR_COORDINATES: in front of the other one
    Cmd(0x0173).I(GetPedRef(me)).F(heading).Run();  // SET_CHAR_HEADING
    Send(OP_GO, a.who, a.id);
    // the other side starts as our word reaches it: we start half a round trip later
    Start(*e, e->other ? e->other : e->main, a.who, NowMs() + std::min<uint32_t>(Net().Rtt() / 2, 300));
    LOGI("Pair emote %u with %s: yes, placed in front of it", a.id, Net().PeerName(a.who).c_str());
    return true;
}

std::string Text(std::string& hint) {
    if (g_asking.who == 0xFF || NowMs() >= g_asking.until || !Net().Peers().count(g_asking.who)) return "";
    const Emote* e = Find(g_asking.id);
    hint = TR("Y - да, N - нет", "Y: yes, N: no");
    return Format(TR("%s предлагает: %s", "%s offers: %s"), Net().PeerName(g_asking.who).c_str(), e ? e->name.c_str() : "?");
}

bool IsCustomBlock(int block) {
    for (const auto* v : {&g_custom, &g_packs})
        for (const Custom& c : *v)
            if (c.index == block) return true;
    return false;
}

bool NeedBlock(int block) {
    if (block < 0) return false;
    if (auto it = g_ourBlocks.find(block); it != g_ourBlocks.end()) it->second = NowMs();
    if (anim::BlockLoaded(block)) return true;
    RequestModel(anim::kIfpBase + block, 2);
    g_ourBlocks[block] = NowMs();  // ours to let go (ReleaseBlocks)
    return false;
}

std::string CustomFolder() { return WideToUtf8(Cfg().coopDir) + "anims"; }

std::string PacksFolder() { return WideToUtf8(Cfg().coopDir) + "packs"; }

}  // namespace coop::anims

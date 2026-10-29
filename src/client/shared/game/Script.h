#pragma once
// Executes single SCM opcodes from C++ through the game's own interpreter
// (CRunningScript::ProcessOneCommand). This gives access to ~2000 well-tested game commands
// (creating cars/peds, tasks, cutscenes, blips, ...) without re-implementing them.
//
//   int handle;
//   game::script::Cmd(0x00A5).I(model).F(x).F(y).F(z).Out(&handle).Run();   // CREATE_CAR
//
// Opcode numbers follow the Sanny Builder SA database (SASCM.ini / plugin-sdk Commands enum).
#include <cstdint>
#include <cstring>

namespace game::script {

class Cmd {
public:
    explicit Cmd(uint16_t opcode);

    Cmd& I(int32_t v);              // integer literal
    Cmd& F(float v);                // float literal
    Cmd& S(const char* str);        // variable-length string literal (type 0x0E)
    Cmd& S8(const char* str);       // fixed 8-byte string literal (type 0x09), e.g. GXT keys
    Cmd& Out(int32_t* result);      // output into a local variable, copied back after Run()
    Cmd& Out(float* result);
    Cmd& End();                     // end of variadic arguments (START_NEW_SCRIPT etc.)

    // Executes the command. Returns the condition result for conditional opcodes.
    bool Run();

private:
    void Put(const void* p, size_t n);
    uint8_t m_code[256];
    size_t m_size = 0;
    struct OutVar {
        void* ptr;
        uint16_t index;
    };
    OutVar m_outs[16];
    int m_numOuts = 0;
    uint16_t m_nextLocal = 0;
    bool m_overflow = false;
};

// Commonly used opcodes (SA):
enum : uint16_t {
    OP_WAIT = 0x0001,
    OP_SET_CHAR_COORDINATES = 0x00A1,
    OP_CREATE_CHAR = 0x009A,
    OP_DELETE_CHAR = 0x009B,
    OP_CREATE_CAR = 0x00A5,
    OP_DELETE_CAR = 0x00A6,
    OP_SET_CAR_COORDINATES = 0x00AB,
    OP_SET_CAR_HEADING = 0x0175,
    OP_SET_CHAR_HEADING = 0x0173,
    OP_CREATE_OBJECT = 0x0107,
    OP_DELETE_OBJECT = 0x0108,
    OP_SET_CHAR_HEALTH = 0x0223,
    OP_SET_CAR_HEALTH = 0x0224,
    OP_GIVE_WEAPON_TO_CHAR = 0x01B2,
    OP_SET_CURRENT_CHAR_WEAPON = 0x01B9,
    OP_REMOVE_ALL_CHAR_WEAPONS = 0x048F,
    OP_WARP_CHAR_INTO_CAR = 0x036A,
    OP_WARP_CHAR_INTO_CAR_AS_PASSENGER = 0x0430,
    OP_WARP_CHAR_FROM_CAR_TO_COORD = 0x0362,
    OP_TASK_ENTER_CAR_AS_PASSENGER = 0x05CA,
    OP_TASK_ENTER_CAR_AS_DRIVER = 0x05CB,
    OP_TASK_LEAVE_CAR = 0x05CD,
    OP_TASK_LEAVE_CAR_IMMEDIATELY = 0x0622,
    OP_TASK_GO_STRAIGHT_TO_COORD = 0x05D3,
    OP_TASK_STAND_STILL = 0x05BA,
    OP_TASK_PLAY_ANIM = 0x0605,
    OP_CLEAR_CHAR_TASKS = 0x0687,
    OP_CLEAR_CHAR_TASKS_IMMEDIATELY = 0x0792,
    OP_SET_CHAR_PROOFS = 0x02AB,
    OP_SET_CAR_PROOFS = 0x02AC,
    OP_MARK_CHAR_AS_NO_LONGER_NEEDED = 0x01C2,
    OP_MARK_CAR_AS_NO_LONGER_NEEDED = 0x01C3,
    OP_SET_CHAR_AREA_VISIBLE = 0x0860,
    OP_SET_AREA_VISIBLE = 0x04BB,
    OP_LOAD_SCENE = 0x03CB,
    OP_REQUEST_COLLISION = 0x04E4,
    OP_DO_FADE = 0x016A,
    OP_SET_PLAYER_CONTROL = 0x01B4,
    OP_DISPLAY_RADAR = 0x0581,
    OP_SET_TIME_OF_DAY = 0x00C0,
    OP_FORCE_WEATHER_NOW = 0x01B6,
    OP_SET_CHAR_STAY_IN_SAME_PLACE = 0x0350,
    OP_SET_CHAR_NEVER_TARGETTED = 0x0568,
    OP_SET_CHAR_SUFFERS_CRITICAL_HITS = 0x0446,
    OP_SET_CHAR_CAN_BE_KNOCKED_OFF_BIKE = 0x08C6,
    OP_SET_CHAR_CANT_BE_DRAGGED_OUT = 0x039E,
    OP_SET_PLAYER_CAN_DO_DRIVE_BY = 0x0501,
    OP_FREEZE_CHAR_POSITION = 0x04D7,
    OP_SET_CHAR_COLLISION = 0x0619,
    OP_SET_CHAR_DROPS_WEAPONS_WHEN_DEAD = 0x087E,
    OP_SET_CHAR_KEEP_TASK = 0x0961,
    OP_ADD_BLIP_FOR_CHAR = 0x0187,
    OP_CHANGE_BLIP_COLOUR = 0x0165,
    OP_SET_BLIP_AS_FRIENDLY = 0x07E0,
    OP_LOCK_CAR_DOORS = 0x020A,
    OP_CHANGE_CAR_COLOUR = 0x0229,
    OP_SET_CAN_BURST_CAR_TYRES = 0x053F,
    OP_LOAD_CUTSCENE = 0x02E4,
    OP_START_CUTSCENE = 0x02E7,
    OP_HAS_CUTSCENE_LOADED = 0x06B9,
    OP_HAS_CUTSCENE_FINISHED = 0x02E9,
    OP_CLEAR_CUTSCENE = 0x02EA,
    OP_SET_CHAR_DECISION_MAKER = 0x060B,
    OP_SET_CHAR_RELATIONSHIP = 0x077A,
};

// A command's arguments in a mission script's code, of the types CRunningScript reads: where the one after arg starts
// (null for a type not known here), and the place of an output variable written there (a global, or a local of the
// mission; null for anything else)
const uint8_t* NextArg(const uint8_t* arg);
int32_t* OutVar(const uint8_t* arg);
// an argument's value as the command read it: a number in the code or a variable's; false for anything else. After a
// command with outputs ScriptParams hold those, not its inputs.
bool ArgValue(const uint8_t* arg, int32_t& value);

bool Running();  // one of our commands runs now (a game function it calls may ask)
// tests: our commands count as a mission script's (m_bIsMission) while on, as the host's mission would run them
void AsMission(bool on);

}  // namespace game::script

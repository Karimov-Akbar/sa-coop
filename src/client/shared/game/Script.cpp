#include "shared/game/Script.h"

#include "common/lib/log/Log.h"
#include "shared/game/Game.h"

namespace game::script {

namespace {
constexpr size_t kRunningScriptSize = 0xE0;
constexpr uintptr_t kRunningScriptInit = 0x4648E0;        // CRunningScript::Init
constexpr uintptr_t kProcessOneCommand = 0x469EB0;        // CRunningScript::ProcessOneCommand
constexpr uintptr_t kScriptParams = 0xA43C78;             // ScriptParams[32]
constexpr uintptr_t kMissionLocals = 0xA48960;            // CTheScripts::LocalVariablesForCurrentMission
constexpr int kOffBaseIP = 0x10, kOffCurIP = 0x14, kOffLocals = 0x3C, kOffCondResult = 0xC5;
constexpr int kOffUseMissionCleanup = 0xC6, kOffIsMission = 0xDC, kOffName = 0x08;

alignas(16) uint8_t g_script[kRunningScriptSize];
int g_depth = 0;
}  // namespace

Cmd::Cmd(uint16_t opcode) { Put(&opcode, 2); }

void Cmd::Put(const void* p, size_t n) {
    if (m_size + n > sizeof(m_code)) {
        m_overflow = true;
        return;
    }
    memcpy(m_code + m_size, p, n);
    m_size += n;
}

Cmd& Cmd::I(int32_t v) {
    uint8_t t = 1;  // SCRIPTPARAM_STATIC_INT_32BITS
    Put(&t, 1);
    Put(&v, 4);
    return *this;
}

Cmd& Cmd::F(float v) {
    uint8_t t = 6;  // SCRIPTPARAM_STATIC_FLOAT
    Put(&t, 1);
    Put(&v, 4);
    return *this;
}

Cmd& Cmd::S(const char* str) {
    uint8_t t = 0x0E;  // SCRIPTPARAM_STATIC_PASCAL_STRING
    // CRunningScript::ReadTextLabelFromScript copies it unbounded into the handler's stack buffer (24 bytes for an
    // animation name, 16 for its block) and takes a length of 128 or more as negative
    size_t n = strlen(str);
    if (n > 23) {
        LOGE("script::Cmd string too long: %s", str);
        n = 23;
    }
    uint8_t len = (uint8_t)n;
    Put(&t, 1);
    Put(&len, 1);
    Put(str, n);
    return *this;
}

Cmd& Cmd::S8(const char* str) {
    uint8_t t = 0x09;  // SCRIPTPARAM_STATIC_SHORT_STRING
    char buf[8] = {};
    strncpy(buf, str, 7);
    Put(&t, 1);
    Put(buf, 8);
    return *this;
}

Cmd& Cmd::Out(int32_t* result) {
    if (m_numOuts >= 16 || m_nextLocal >= 32) {
        m_overflow = true;
        return *this;
    }
    uint8_t t = 3;  // SCRIPTPARAM_LOCAL_NUMBER_VARIABLE
    Put(&t, 1);
    Put(&m_nextLocal, 2);
    m_outs[m_numOuts++] = {result, m_nextLocal++};
    return *this;
}

Cmd& Cmd::Out(float* result) { return Out(reinterpret_cast<int32_t*>(result)); }

Cmd& Cmd::End() {
    uint8_t t = 0;
    Put(&t, 1);
    return *this;
}

const uint8_t* NextArg(const uint8_t* a) {
    switch (a[0]) {
        case 0x01: case 0x06: return a + 5;                                               // int32, float
        case 0x02: case 0x03: case 0x0A: case 0x0B: case 0x10: case 0x11: return a + 3;  // variables
        case 0x04: return a + 2;                                                          // int8
        case 0x05: return a + 3;                                                          // int16
        case 0x07: case 0x08: case 0x0C: case 0x0D: case 0x12: case 0x13: return a + 7;  // arrays
        case 0x09: return a + 9;                                                          // 8-byte string
        case 0x0E: return a + 2 + a[1];                                                   // string of a length
        case 0x0F: return a + 17;                                                         // 16-byte string
    }
    return nullptr;
}

int32_t* OutVar(const uint8_t* a) {
    if (!a) return nullptr;
    uint16_t n = *(const uint16_t*)(a + 1);
    if (a[0] == 0x02) return (int32_t*)(addr::ScriptSpace + n);
    if (a[0] == 0x03) return (int32_t*)(kMissionLocals + (uintptr_t)n * 4);  // CTheScripts::LocalVariablesForCurrentMission
    return nullptr;
}

bool ArgValue(const uint8_t* a, int32_t& value) {
    if (!a) return false;
    switch (a[0]) {
        case 0x01: return value = *(const int32_t*)(a + 1), true;
        case 0x04: return value = (int8_t)a[1], true;
        case 0x05: return value = *(const int16_t*)(a + 1), true;
    }
    int32_t* var = OutVar(a);
    return var && (value = *var, true);
}

bool Running() { return g_depth > 0; }

bool g_asMission = false;
void AsMission(bool on) { g_asMission = on; }

bool Cmd::Run() {
    if (m_overflow) {
        LOGE("script::Cmd overflow (opcode %04X)", *(uint16_t*)m_code);
        return false;
    }
    // padding so a buggy handler that over-reads does not run into garbage
    uint8_t zero[8] = {};
    Put(zero, sizeof(zero));

    // Our commands may be issued from inside hooks that run in the middle of another script
    // command (e.g. CMessages::AddMessage called by a mission). The game keeps collected
    // parameters in the global ScriptParams array, so preserve it.
    uint32_t savedParams[32];
    memcpy(savedParams, (void*)kScriptParams, sizeof(savedParams));
    uint8_t savedScript[kRunningScriptSize];
    bool nested = g_depth++ > 0;
    if (nested) memcpy(savedScript, g_script, sizeof(g_script));

    memset(g_script, 0, sizeof(g_script));
    CallMethod<kRunningScriptInit>(reinterpret_cast<void*>(g_script));
    memcpy(g_script + kOffName, "coop", 5);
    g_script[kOffIsMission] = g_asMission ? 1 : 0;
    g_script[kOffUseMissionCleanup] = 0;
    Field<uint8_t*>(g_script, kOffBaseIP) = m_code;
    Field<uint8_t*>(g_script, kOffCurIP) = m_code;

    CallMethod<kProcessOneCommand, int8_t>(reinterpret_cast<void*>(g_script));

    for (int i = 0; i < m_numOuts; ++i)
        *reinterpret_cast<int32_t*>(m_outs[i].ptr) = Field<int32_t>(g_script, kOffLocals + m_outs[i].index * 4);
    bool cond = g_script[kOffCondResult] != 0;

    if (nested) memcpy(g_script, savedScript, sizeof(g_script));
    g_depth--;
    memcpy((void*)kScriptParams, savedParams, sizeof(savedParams));
    return cond;
}

}  // namespace game::script

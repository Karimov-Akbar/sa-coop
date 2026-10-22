// The radio of a car with players in it. Each game plays its own radio (CAERadioTrackManager, one per game), and the
// remote driver's car is not its player's for this game's vehicle sound (see HookVehicleAudioService in Players.cpp),
// so a passenger heard the car's own station picked at random here, at a random point of a random track, and could
// retune it. The driver's game sends its radio while players ride with it: the station, the tracks queued on it and
// where the playing one is. The passenger's game tunes in to the same station, puts the same tracks in the queue and
// starts the playing one at the driver's point; its own retune buttons are dead while the driver is a remote player.
// The User Track Player plays files of the player's own computer: a passenger's plays its own tracks (the files are not
// sent over the network), and the chat says so.
#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "entities/vehicle/Vehicles.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/lib/memory/Memory.h"
#include "shared/ui/chat/Chat.h"

namespace coop {

namespace {
using namespace game;

constexpr uintptr_t kRadio = 0x8CB6F8;          // AERadioTrackManager (CAERadioTrackManager, 0x370)
constexpr uintptr_t kAudioEngine = 0xB6BC90;    // AudioEngine
// CAEVehicleAudioEntity::s_pVehicleAudioSettingsForRadio: the settings of the car this game's player rides in, set by
// TurnOnRadioForVehicle (0x4F5B20) for the driver and the passengers alike
constexpr uintptr_t kRadioCarSettings = 0xB6B99C;
constexpr uintptr_t kUserTracksCount = 0xB6B970 + 0x06;  // AEUserRadioTrackManager.m_nUserTracksCount

// CAERadioTrackManager
constexpr int kMode = 0x68;                 // m_nMode (eRadioTrackMode)
constexpr int kStationsListed = 0x6C;       // m_nStationsListed: retune presses not acted on yet
constexpr int kStationsListDown = 0x70;
constexpr int kMenuRequest = 0x78;          // m_iRadioStationMenuRequest: RetuneRadio while paused
constexpr int kScriptRequest = 0x7C;        // m_iRadioStationScriptRequest: RetuneRadio otherwise
constexpr int kRequested = 0x88;            // m_RequestedSettings (tRadioSettings, 0x3C): what Service plays next
constexpr int kActive = 0xC4;               // m_ActiveSettings: what plays now
// tRadioSettings
constexpr int kQueue = 0x00;                // TrackQueue[5]: the playing track first
constexpr int kPlayingTrack = 0x14;         // CurrTrackID: AEAudioHardware.GetPlayingTrackID, every Service
constexpr int kPlayTime = 0x1C;             // PlayTime: requested: where the first track starts; active: where it is
constexpr int kTrackLength = 0x20;          // TrackLengthMs
constexpr int kFlags = 0x24;                // TrackFlags (for AEAudioHardware.PlayTrack)
constexpr int kStation = 0x25;              // StationID
constexpr int kBass = 0x26;                 // BassSetting
constexpr int kBassGain = 0x28;             // BassGain
constexpr int kTypes = 0x2C;                // TrackTypes[5]
constexpr int kIndices = 0x33;              // TrackIndices[5]
constexpr int kQueueLength = 5;

enum : int8_t { RADIO_USER_TRACKS = 12, RADIO_OFF = 13 };
enum : int32_t { MODE_PLAYING = 2 };
enum : uint8_t { RADIO_TYPE_CIVILIAN = 0 };
enum : int { AE_FRONTEND_RADIO_RETUNE_START = 0x22, AE_FRONTEND_RADIO_CLICK_ON = 0x25, AE_FRONTEND_RADIO_CLICK_OFF = 0x26 };

constexpr uint64_t kResendMs = 3000;     // the driver's correction for the passengers
constexpr uint64_t kStaleMs = 10000;     // the driver's radio is not known any more
constexpr int32_t kToleranceMs = 1000;   // a passenger this far from the driver's point starts the track again
constexpr uint64_t kSettleMs = 5000;     // a retune of ours gets this long to start playing

uint8_t* Radio() { return (uint8_t*)kRadio; }
template <typename T> T& Requested(int off) { return Field<T>(Radio(), kRequested + off); }
template <typename T> T& Active(int off) { return Field<T>(Radio(), kActive + off); }

// a civilian radio plays in the car this game's player rides in (an emergency one plays the police scanner)
bool CarRadio() {
    auto* settings = Ref<uint8_t*>(kRadioCarSettings);
    return settings && settings[0x1B] == RADIO_TYPE_CIVILIAN;  // tVehicleAudioSettings::RadioType
}

void FrontendSound(int event) {
    CallMethod<0x506EA0>((void*)kAudioEngine, event, 0.0f, 1.0f);  // CAudioEngine::ReportFrontendAudioEvent
}

// what a driver's radio plays
struct RadioState {
    uint16_t veh = 0;
    int8_t station = RADIO_OFF;
    int n = 0;  // tracks known (none for the radio off or the User Track Player)
    int32_t tracks[kQueueLength];
    int8_t types[kQueueLength], indices[kQueueLength];
    uint8_t flags = 0;
    int32_t pos = -1, length = 0;  // ms into the first track, its length
    uint64_t recvMs = 0;
    bool fresh = false;            // not compared with this game's radio yet
};

// ---- the driver ----

// this game's radio as the driver's message has it; false while it retunes
bool ReadLocal(RadioState& s) {
    s.station = Requested<int8_t>(kStation);  // the station it goes to, set at once by StartRadio
    s.n = 0;
    s.pos = -1;
    if (s.station == RADIO_OFF || s.station == RADIO_USER_TRACKS) return true;
    if (Field<int32_t>(Radio(), kMode) != MODE_PLAYING || Active<int8_t>(kStation) != s.station) return false;
    // PlayTime is the stream's position (CAERadioTrackManager::Service's argument, AEAudioHardware.GetTrackPlayTime)
    // in the playing track, or a negative state while it loads; the queue moves on a little after the stream does
    // (CheckForTrackConcatenation)
    s.pos = Active<int32_t>(kPlayTime);
    if (s.pos < 0 || Active<int32_t>(kPlayingTrack) != Active<int32_t>(kQueue)) return false;
    for (; s.n < kQueueLength && Active<int32_t>(kQueue + s.n * 4) >= 0; ++s.n) {
        s.tracks[s.n] = Active<int32_t>(kQueue + s.n * 4);
        s.types[s.n] = Active<int8_t>(kTypes + s.n);
        s.indices[s.n] = Active<int8_t>(kIndices + s.n);
    }
    s.flags = Active<uint8_t>(kFlags);
    s.length = Active<int32_t>(kTrackLength);
    return s.n > 0;
}

RadioState g_sent;
std::vector<uint8_t> g_sentTo;  // the players who rode with us when we last sent
uint64_t g_sentMs = 0;

void DriverUpdate() {
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me ? me->VehicleIfIn() : nullptr;
    NetVehicle* nv = v && v->Driver() == me && CarRadio() ? Vehicles::Instance().Find(v) : nullptr;
    std::vector<uint8_t> riders;
    if (nv)
        for (auto& [id, rp] : Players::Instance().All())
            if (rp.ped && rp.ped->VehicleIfIn() == v) riders.push_back(id);
    if (riders.empty()) {
        g_sentTo.clear();
        return;
    }
    RadioState s;
    s.veh = nv->netId;
    if (!ReadLocal(s)) return;
    bool changed = s.veh != g_sent.veh || s.station != g_sent.station || (s.n && s.tracks[0] != g_sent.tracks[0]) ||
                   riders != g_sentTo;
    if (!changed && NowMs() - g_sentMs < kResendMs) return;
    if (changed) LOGI("Radio of our car %04X for its passengers: station %d, track %d", s.veh, s.station, s.n ? s.tracks[0] : -1);
    g_sent = s;
    g_sentTo = riders;
    g_sentMs = NowMs();
    Writer w(proto::G_RADIO);
    w.U16(s.veh);
    w.I8(s.station);
    w.U8((uint8_t)s.n);
    for (int i = 0; i < s.n; ++i) {
        w.I32(s.tracks[i]);
        w.I8(s.types[i]);
        w.I8(s.indices[i]);
    }
    w.U8(s.flags);
    w.I32(s.pos);
    w.I32(s.length);
    Net().Send(proto::CH_RELIABLE, w);
}

// ---- the passenger ----

std::map<uint8_t, RadioState> g_drivers;  // the latest radio of each player who drives with players in the car

// the remote player driving the car this game's player rides in as a passenger
RemotePlayer* RemoteDriver() {
    if (!Net().Online()) return nullptr;
    CPlayerPed* me = FindPlayerPed(0);
    CVehicle* v = me ? me->VehicleIfIn() : nullptr;
    CPed* driver = v ? v->Driver() : nullptr;
    return driver && driver != me ? Players::Instance().FindByPed(driver) : nullptr;
}

// a retune of ours that has not started playing yet
bool g_pending = false;
uint64_t g_pendingMs = 0;
int32_t g_pendingTrack = -1;
bool g_pendingSync = false;  // it starts the driver's track at the driver's point
RadioState g_pendingState;   // the driver's radio it was started for
uint64_t g_playTrackMs = 0;
int32_t g_loadMs = 250;  // from AEAudioHardware.PlayTrack to the first sound, as last measured
// a driver's track this game was put right on, how many times: a stream that does not keep the point would restart
// the track every few seconds
int32_t g_fixedTrack = -1;
int g_fixes = 0;
constexpr int kMaxFixes = 2;

// where the driver's first track is now
int32_t DriverPos(const RadioState& s) {
    return s.pos + (int32_t)(NowMs() - s.recvMs) + (int32_t)Net().Rtt();  // its way here: about both ends' half pings
}

// the driver's station on this game's radio, the driver's tracks queued from its point (none: the game's choice)
void Tune(int8_t station, const RadioState* s) {
    int8_t was = Active<int8_t>(kStation);
    // CAERadioTrackManager::StartRadio(id, bass, bassGain, skipTrack) as CheckForStationRetune calls it: the tracks
    // go into m_RequestedSettings, and Service stops the playing one and starts them
    CallMethod<0x4EB3C0>(Radio(), station, Active<int8_t>(kBass), Active<float>(kBassGain), (uint8_t)0);
    if (Requested<int8_t>(kStation) != station) return;  // a mission's ambience track holds the radio
    if (station != was) {
        FrontendSound(station == RADIO_OFF ? AE_FRONTEND_RADIO_CLICK_OFF
                      : was == RADIO_OFF   ? AE_FRONTEND_RADIO_CLICK_ON
                                           : AE_FRONTEND_RADIO_RETUNE_START);
        Field<uint8_t>(Radio(), 0x01) = 1;  // m_bDisplayStationName
    }
    g_pending = true;
    g_pendingMs = NowMs();
    g_pendingSync = s != nullptr;
    g_pendingTrack = -1;
    if (!s) return;
    g_pendingState = *s;
    int first = 0;  // the driver's track is over by now: the next one
    if (s->n > 1 && DriverPos(*s) > s->length - 1000) first = 1;
    for (int i = 0; i < kQueueLength; ++i) {
        bool known = first + i < s->n;
        Requested<int32_t>(kQueue + i * 4) = known ? s->tracks[first + i] : -1;
        Requested<int8_t>(kTypes + i) = known ? s->types[first + i] : 6;  // TYPE_NONE
        Requested<int8_t>(kIndices + i) = known ? s->indices[first + i] : -1;
    }
    Requested<uint8_t>(kFlags) = s->flags;
    Requested<int32_t>(kPlayTime) = 0;  // set again as the track starts (HookPlayTrack)
    g_pendingTrack = s->tracks[first];
}

bool g_toldUserTracks = false;  // the chat said the driver's User Track Player is not heard here

void PassengerUpdate() {
    if (g_pending && (Field<int32_t>(Radio(), kMode) == MODE_PLAYING || NowMs() - g_pendingMs > kSettleMs)) {
        if (g_playTrackMs && Field<int32_t>(Radio(), kMode) == MODE_PLAYING)
            g_loadMs = std::clamp((int32_t)(NowMs() - g_playTrackMs), 0, 1500);
        g_pending = g_pendingSync = false;
        g_playTrackMs = 0;
    }
    RemotePlayer* rp = RemoteDriver();
    auto it = rp && CarRadio() ? g_drivers.find(rp->id) : g_drivers.end();
    NetVehicle* nv = rp ? Vehicles::Instance().Find(rp->ped->VehicleIfIn()) : nullptr;
    if (it == g_drivers.end() || !nv || it->second.veh != nv->netId || NowMs() - it->second.recvMs > kStaleMs) {
        g_toldUserTracks = false;
        return;
    }
    RadioState& s = it->second;
    if (s.station != RADIO_USER_TRACKS) {
        g_toldUserTracks = false;
    } else if (!g_toldUserTracks) {
        g_toldUserTracks = true;
        ui::TheChat().AddSystem(Ref<uint16_t>(kUserTracksCount)
                                    ? TR("Водитель слушает свою музыку, а у вас играет ваш User Track Player.",
                                         "The driver listens to their own music; your User Track Player plays for you.")
                                    : TR("Водитель слушает свою музыку. У вас своих треков нет, поэтому радио выключено.",
                                         "The driver listens to their own music. You have no tracks of your own, so the radio is off."));
    }
    int8_t want = s.station;
    if (want == RADIO_USER_TRACKS && !Ref<uint16_t>(kUserTracksCount)) want = RADIO_OFF;  // no tracks of our own
    int8_t mine = Requested<int8_t>(kStation);
    if (g_pending && mine == want) return;
    if (want == RADIO_OFF || want == RADIO_USER_TRACKS || s.n == 0) {
        if (mine == want) return;
        LOGI("Radio: player %d's station %d", rp->id, want);
        Tune(want, nullptr);
        return;
    }
    if (mine == want) {
        if (!s.fresh) return;  // compared once per message: the driver's point moves on with its game's clock
        s.fresh = false;
        int32_t at = DriverPos(s);
        if (at > s.length - 3000) return;  // it is about to move on to the next track of the same queue
        int32_t track = Active<int32_t>(kQueue), pos = Active<int32_t>(kPlayTime);
        if (track == s.tracks[0] && pos >= 0 && std::abs(pos - at) < kToleranceMs) return;
        if (g_fixedTrack != s.tracks[0]) g_fixedTrack = s.tracks[0], g_fixes = 0;
        if (g_fixes >= kMaxFixes) return;
        if (++g_fixes == kMaxFixes) LOGW("Radio: track %d kept missing the driver's point, left as it is", track);
        LOGD("Radio: track %d at %d ms, player %d's track %d at %d ms", track, pos, rp->id, s.tracks[0], at);
    } else {
        LOGI("Radio: player %d's station %d, track %d", rp->id, want, s.tracks[0]);
    }
    s.fresh = false;
    Tune(want, &s);
}

void Handle(uint8_t from, Reader& r) {
    RadioState s;
    s.veh = r.U16();
    s.station = r.I8();
    s.n = r.U8();
    if (s.n > kQueueLength) return;
    for (int i = 0; i < s.n; ++i) {
        s.tracks[i] = r.I32();
        s.types[i] = r.I8();
        s.indices[i] = r.I8();
    }
    s.flags = r.U8();
    s.pos = r.I32();
    s.length = r.I32();
    if (!r.Ok() || s.station <= 0 || s.station > RADIO_OFF) return;  // the police scanner (0) is not sent
    if (s.n && (s.pos < 0 || s.tracks[0] < 0)) return;
    s.recvMs = NowMs();
    s.fresh = true;
    g_drivers[from] = s;
}

// ---- hooks ----

// CAERadioTrackManager::Service -> CheckForStationRetune: the retune buttons (CPad::ChangeStationJustDown/UpJustDown)
// and RetuneRadio's requests (the scripts, the menu's audio page). A passenger of a remote driver keeps its station.
using RetuneFn = void(__fastcall*)(void*, void*);
RetuneFn g_origRetune = nullptr;
void __fastcall HookCheckForStationRetune(void* radio, void* edx) {
    if (!RemoteDriver()) return g_origRetune(radio, edx);
    Field<int32_t>(radio, kStationsListed) = Field<int32_t>(radio, kStationsListDown) = 0;
    Field<int32_t>(radio, kScriptRequest) = -1;
    Field<uint8_t>(radio, 0x06) = 0;  // m_bRetuneJustStarted
}

// -> CheckForStationRetuneDuringPause: the menu's choice while the game is paused
RetuneFn g_origRetunePaused = nullptr;
void __fastcall HookCheckForStationRetuneDuringPause(void* radio, void* edx) {
    if (!RemoteDriver()) return g_origRetunePaused(radio, edx);
    Field<int32_t>(radio, kMenuRequest) = -1;
}

// CAERadioTrackManager::Service (mode RADIO_STARTING) -> AEAudioHardware.PlayTrack(track, next, startOffsetMs, flags,
// userTrack, nextUserTrack): our retune starts the driver's track where the driver is by now, ahead by the time the
// stream takes to start it
using PlayTrackFn = void(__fastcall*)(void*, void*, uint32_t, int32_t, uint32_t, uint8_t, uint8_t, uint8_t);
PlayTrackFn g_origPlayTrack = nullptr;
void __fastcall HookPlayTrack(void* hw, void* edx, uint32_t track, int32_t next, uint32_t offset, uint8_t flags,
                              uint8_t user, uint8_t userNext) {
    if (g_pending && g_pendingSync && g_pendingTrack >= 0 && (int32_t)track == g_pendingTrack) {
        const RadioState& s = g_pendingState;
        int32_t at = track == (uint32_t)s.tracks[0] ? DriverPos(s) : DriverPos(s) - s.length;
        offset = (uint32_t)std::clamp(at + g_loadMs, 0, std::max(s.length - 1000, 0));
        g_playTrackMs = NowMs();
    }
    g_origPlayTrack(hw, edx, track, next, offset, flags, user, userNext);
}

class SharedRadio : public Module {
public:
    const char* Name() const override { return "SharedRadio"; }

    void OnInstallHooks() override {
        mem::RedirectCallFn(0x4EB9F4, &HookCheckForStationRetune, &g_origRetune);
        mem::RedirectCallFn(0x4EB9FD, &HookCheckForStationRetuneDuringPause, &g_origRetunePaused);
        mem::RedirectCallFn(0x4EBB11, &HookPlayTrack, &g_origPlayTrack);
        Net().SetHandler(proto::G_RADIO, [](uint8_t from, Reader& r) { Handle(from, r); });
    }
    void OnSessionStart() override { Reset(); }
    void OnSessionEnd() override { Reset(); }
    void OnGameShutdown() override { Reset(); }
    void OnPeerLeave(uint8_t id) override { g_drivers.erase(id); }

    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        DriverUpdate();
        PassengerUpdate();
    }

    static SharedRadio& Instance() {
        static SharedRadio m;
        return m;
    }

private:
    static void Reset() {
        g_drivers.clear();
        g_sentTo.clear();
        g_sent = {};
        g_pending = g_pendingSync = false;
        g_playTrackMs = 0;
        g_toldUserTracks = false;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&SharedRadio::Instance()); }
} g_reg;
}  // namespace

}  // namespace coop

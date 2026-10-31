#pragma once
// SA:COOP network protocol. See docs/PROTOCOL.md.
//
// Every ENet packet starts with a one-byte message id.
//  * ids < 64  — service messages, handled by the server itself;
//  * ids >= 64 — game messages: the client sends [id][payload], the server relays them to the other
//    players as [id][u8 senderId][payload]. The server never parses game payloads.
#include <cstdint>
#include <string>
#include <vector>

#include "common/lib/buffer/Buffer.h"

namespace coop::proto {

constexpr int kMaxPlayers = 4;  // hard protocol limit
// what the game client supports: the other player's character takes the game's second player slot (CWorld::Players[1]),
// and a third player's could not be made (fires, props and trains are keyed by the sender's own ids too)
constexpr int kGamePlayers = 4;
constexpr uint8_t kServerId = 255;
constexpr size_t kMaxChatLen = 160;
constexpr size_t kWorldChunk = 16 * 1024;

enum Channel : uint8_t {
    CH_RELIABLE = 0,  // ordered, reliable: events
    CH_STATE = 1,     // unreliable sequenced: positions/states
    CH_VOICE = 2,     // unreliable: voice frames
    CH_BULK = 3,      // ordered, reliable: file transfers (save game)
    CH_COUNT = 4,
};

enum MsgId : uint8_t {
    // ---- client -> server ----
    C_HELLO = 1,
    C_CHAT = 2,              // str text (may be a /command)
    C_PROFILE_SAVE = 3,      // bytes profile (opaque for the server)
    C_WORLD_BEGIN = 4,       // host uploads its save: u32 size, u32 crc, str label
    C_WORLD_CHUNK = 5,       // u32 offset, bytes
    C_WORLD_END = 6,
    C_WORLD_REQUEST = 7,     // ask the server to send the current world save
    C_STATUS = 8,            // u8 ClientStatus
    C_WORLD_KEEP = 9,        // host: a fresh snapshot is impossible now (mission), send the stored world
    C_WORLD_NEWGAME = 10,    // host: it plays a new game's prologue (no save of it can exist yet)
    C_WORLD_FOLLOW = 11,     // host: the prologue is over but cannot be saved yet: partners follow in a new game

    // ---- server -> client ----
    S_WELCOME = 20,          // see Welcome
    S_REJECT = 21,           // str reason, str reasonEn (the same in English; empty when there is nothing to translate)
    S_PLAYER_JOIN = 22,      // u8 id, str name, u8 isHost
    S_PLAYER_LEAVE = 23,     // u8 id, u8 reason
    S_CHAT = 24,             // u8 fromId (255 = server), u32 rgba, str text, str textEn (the server's own texts)
    S_PROFILE = 25,          // bytes profile (empty when the player is new)
    S_WORLD_BEGIN = 26,      // u32 size, u32 crc, str label
    S_WORLD_CHUNK = 27,      // u32 offset, bytes
    S_WORLD_END = 28,
    S_WORLD_NONE = 29,       // the host has not uploaded a save yet
    S_HOST_CHANGED = 30,     // u8 newHostId
    S_PINGS = 31,            // u8 count, {u8 id, u16 ping}
    S_KICKED = 32,           // str reason, str reasonEn
    S_WORLD_WANTED = 33,     // to the host: a partner needs the world, please upload a fresh snapshot
    S_INVITE = 34,           // str invite code for internet play (when the server learned its public address)
    S_WORLD_NEWGAME = 35,    // to a partner: start a new game and join the host where the prologue left it
    S_WORLD_WAIT = 36,       // to a partner: the host plays a new game's prologue, keep waiting

    // ---- relayed game messages (client <-> client) ----
    G_FIRST = 64,
    G_PLAYER_ONFOOT = 64,     // unreliable
    G_PLAYER_DRIVER = 65,     // unreliable
    G_PLAYER_PASSENGER = 66,  // unreliable
    G_PLAYER_AIM = 67,        // unreliable
    G_PLAYER_SPAWN = 68,      // reliable: (re)spawn, model, interior
    G_PLAYER_DEATH = 69,      // reliable: i16 the default group's animation the sender's player fell with (its copies die so)
    G_PLAYER_LOOK = 70,       // reliable: clothes, fat, muscle, u16 model, the special character's name (290-299),
                              // u8 fighting style, u8 its moves
    G_PLAYER_STATS = 71,      // reliable: stats affecting the remote ped (skills, stamina, fight style)
    G_PLAYER_WEAPONS = 72,    // reliable: weapon inventory
    G_BULLET = 73,            // unreliable: the bullets the sender's player fired from a vehicle this frame, drawn on its copy
                              // unless the copy fired itself (a drive-by mod, #76, Shots.cpp): u8 n, n x (vec3 from, vec3 to,
                              // u8 weapon)
    G_DAMAGE = 74,            // damage event addressed to the owner of the victim
    G_EXPLOSION = 75,         // reliable: a blast went off (creator kind and id, type, position...), see Fire.cpp
    G_PROJECTILE = 76,        // reliable: a grenade, a rocket... was thrown (creator, weapon, position, speed, target)
    G_PLAYER_ENTER_CAR = 77,
    G_PLAYER_EXIT_CAR = 78,
    G_PLAYER_ANIM = 79,       // reliable: one-shot animations (eating, drinking, gestures)

    G_VEH_CREATE = 80,
    G_VEH_REMOVE = 81,
    G_VEH_SYNC = 82,          // unreliable
    G_VEH_OWNER = 83,         // ownership change request/announce
    G_VEH_STATUS = 84,        // reliable: damage, colours, mods, locks, siren
    G_VEH_HANDOVER = 85,      // reliable: u16 netId, u8 to: the owner gives its car away (its peds go before it)
    G_VEH_SCRIPT_FREE = 86,   // reliable: u16 netId: a script of this game let go of its car (a mirror here)
    G_PED_CREATE = 90,
    G_PED_REMOVE = 91,
    G_PED_SYNC = 92,          // unreliable
    G_PED_OWNER = 93,         // reliable: u16 netId, u8 to: the owner gives its ped away (the mirror becomes its own)
    G_PED_STATUS = 94,        // reliable: death, weapon, task hints
    G_RESYNC = 95,            // reliable: "my game (re)started", the others announce their vehicles and peds again
    G_OBJ_CREATE = 100,       // reliable: an object of the host's mission (its last u8: 1 liftable, 2 in the host's hands)
    G_OBJ_REMOVE = 101,       // reliable: it is gone (or broke)
    G_OBJ_SYNC = 102,         // unreliable: it moved (and the same flags byte, i8 its interior)
    G_PICKUP_EVENT = 103,     // reliable: a pickup was collected, a tag painted; host: the tags and collectibles as they are
                              // (to a restarted partner), its mission's SET_TAG_STATUS_IN_AREA
    G_PICKUP_CREATE = 104,    // reliable: a pickup appeared (loot, the host's mission)
    G_OBJ_DAMAGE = 105,       // reliable: the partner hit a mirror object, its owner applies it
    G_FIRE = 106,             // reliable: a fire started (a script's on a car marked) / went out
    G_ZONE = 107,             // reliable: gang densities of a territory
    G_CHEAT = 108,            // reliable: a cheat that changes the shared world
    G_ENEX = 109,             // reliable: entrances the host's scripts opened or closed

    G_WORLD_STATE = 110,      // time, weather, gravity... (host -> all)
    G_WANTED = 111,

    G_MISSION_START = 120,
    G_MISSION_END = 121,
    G_TEXT = 122,             // subtitles / help / big messages
    G_BLIP = 123,             // add/update/remove radar blip
    G_MARKER = 124,           // 3d markers, checkpoints, script spheres
    G_ONSCREEN = 125,         // timers and counters
    G_FADE = 126,
    G_CUTSCENE = 127,         // load/start/finish/skip
    G_SCRIPT_VARS = 128,      // story progress (script global variables)
    G_MISSION_REWARD = 129,
    G_TELEPORT = 130,         // teleport request/command
    G_GAME_EVENT = 131,       // misc one-shot events (audio, widescreen, camera shake)
    G_MISSION_WAIT = 132,     // the host stands in a mission marker and waits for the partner
    G_WAYPOINT = 133,         // reliable: the player's map waypoint (set or cleared)
    G_PROPERTY = 134,         // reliable: the player bought a property (its index in main.scm)
    G_HOST_SAVED = 135,       // reliable: the host saved its game, the partners save their profiles
    G_MISSION_WEAPON = 136,   // reliable: u8 op (1 give, 2 take), u8 weapon, i32 ammo (CJ's count after the change):
                              // the host's mission armed its CJ
    G_CHECKPOINTS = 137,      // reliable: the host's script checkpoints and spheres (u8 n, records)
    G_WORLD_OBJ = 138,        // a map object broke or moved (u16 model, origin, stage, flags, weapon[, pose, speeds]);
                              // reliable for changes, unreliable while it moves
    G_GARAGE = 139,           // reliable: u8 op, u8 garage: the partner's cars into the host's safe house store
                              // (u8 n, u16 net ids), the host has them (same), bring the stored ones out
    G_VEH_CLEAR = 140,        // reliable: u8 n, u16 net ids: the host's mission cleared an area (CLEAR_AREA), the
                              // owners delete these empty cars of theirs there
    G_WATER_CANNON = 141,     // unreliable: u16 vehicle, the jet's offset from it and speed (a fire truck shoots water)
    G_TRAIN = 142,            // a train of the sender's game (u8 op, u16 id; state: kind, track, direction, rails, speed,
                              // pos), unreliable; gone: reliable; drive (unreliable) and enter (reliable): the sender's
                              // player at the wheel of its copy of the train of player u8 (rails, speed)
    G_MISSION_AUDIO = 143,    // reliable, host -> partner: u8 op, u8 slot of the mission's speech (load: i32 sample;
                              // attach: u8 kind, u16 net id; position: vec3; play; clear)
    G_GARAGE_DOOR = 144,      // reliable: u8 garage, u8 door of a shop garage (0 shut, 1 open, 2 none: the sender's
                              // door follows ours)
    G_PROP = 145,             // reliable: u8 op, u16 id of an object the sender's player or ped holds or carries
                              // (state: model, holder, how it hangs, look, own animation; gone; hit: u8 its owner,
                              // f32 damage, u8 weapon, the sender's shot at its copy of a joint range's target)
    G_PED_SPEECH = 146,       // reliable: u8 player/ped/vehicle, u16 net id, the line it starts (context, bank, sound,
                              // delay, volume, forced); a vehicle's: a police helicopter's or boat's loudspeaker
    G_MISSION_POLICY = 147,   // reliable, host -> partner: the mission's police rules (u8 max wanted, u8 back-off bits,
                              // f32 wanted multiplier), u8 city level (STAT_CITY_UNLOCKED), u8 1: its stars were cleared,
                              // f32 density of people, f32 density of cars
    G_VEH_SCRIPT = 148,       // reliable, host -> the driver's game: the host's mission moved, turned or froze a car that
                              // the other player drives (u16 car, u8 its owner, u16 command, its three parameters)
    G_INTERIOR_POP = 149,     // reliable, host -> partner: i8 area, vec3 at: its game made that interior's people
    G_MAP_EDIT = 150,         // reliable, host -> partner: u16 command, vec3, f32 radius, i32 model, i32 state or model: a
                              // map edit of the host's mission (an object's collision or visibility, a building's model)
    G_PHOTO = 151,            // reliable, partner -> host: u8 n, u16 peds, u8 m, u16 mission objects: the host's own that
                              // the partner's camera took (their copies there), for the mission's photo checks
    G_IPL = 152,              // reliable, host -> partner: u8 op (0 in, 1 out, 2 out when far), str name: a part of the
                              // map the host's mission brings in or takes out (REQUEST_IPL / REMOVE_IPL...)
    G_HELI_SHOT = 153,        // reliable: u16 helicopter, vec3 gun from it, vec3 aimed at, u8 damage: a round of a police
                              // helicopter of the sender's game
    G_CONTACTS = 154,         // reliable, host -> partner: u8 n, the story's mission start points of the host's radar
                              // (vec3, u8 sprite, u8 flags, u8 display, u32 colour, u16 size, i16 entrance)
    G_GANG_WAR = 155,         // reliable: u8 fought: a gang war of the sender's game is being fought (sent on a change and
                              // every 3 s while it is), u8 flags (host: 1 wars on, 2 the story holds them, 4 on a mission,
                              // 8 a defence), u16 the district fought for (0xFFFF none)
    G_SIDE = 156,             // reliable: u8 op (1 offer, 2 accept, 3 decline, 4 end, 5 leave, 6 pay, 7 drop, 8 resync, 9
                              // loot, 10 taken, 11 alarm), u8 to (a player, 255 all), str name (the side activity's mission
                              // script), i32 money (pay: a fare as it came, both ways while who joined a taxi job drives its
                              // own; end: the rest it paid; taken: the thing's model); drop: vec3 the leader's courier ring
                              // its package went into; resync: who joined is back from its own taxi job, the leader sends all
                              // it shows again; loot: who joined a burglary put a thing into the van; taken: vec3 the place
                              // of a house's thing the sender lifted; alarm: vec3 where the residents woke for the sender
    G_SCENE_CAM = 157,        // host -> partner, a scene of its story mission on the engine: u8 op 1 sample (state
                              // channel): u8 scene, u8 shot, u32 host ms, vec3 camera, vec3 front, f32 fov, i8 area, u8
                              // flags (1 wide, 2 control off); u8 op 2 end (reliable): u8 scene
    G_TURF_WAR = 158,         // reliable: a gang war the partner's game fights: u8 op (1 start, 2 zone, 3 end, 4 stop), u16
                              // zone info, then start: u16 navigation zone, u8 gang; zone: 16 zone bytes; end: u8 result
    G_STAT_SYNC = 159,        // reliable: u8 full (0 changes, 1 all records: answer, 2 the answer), u8 n, n x {u16 stat,
                              // u8 rule (1 add, 2 max, 3 min non-zero, 4 set: host -> partner), u32 value}
    G_SIDE_DONE = 160,        // reliable: u8 n, n x {u16 global, i32 value}: side jobs and challenges done in the sender's
                              // game (the receiver sets the flag and gives its reward)
    G_BEAT = 161,             // reliable, host -> partner: a music minigame of its story mission: u8 op 1 lowrider (i16 track,
                              // u8 kind, vec3 the host's car, f32 heading), 2 play, 3 stop, 5 dance (u8 streamed script,
                              // vec3 spot, f32 heading, i16 track, f32[30] its five fixed cameras: x, y, z, then aim x, y, z)
    G_BEAT_SCORE = 162,       // reliable, partner -> host: u8 op (1 joined, 2 points, 3 done), u8 kind, i32 beat, i32 points
                              // (the lowrider: this beat's; the dance: its running total)
    G_MISSION_IFP = 165,      // reliable, host -> partner: u8 1 the story mission loads an animation file (REQUEST_ANIMATION),
                              // 0 lets it go; str its name: the partner keeps it in memory too, for its scenes' gestures
    G_MISSION_FACE = 166,     // reliable, host -> partner: a scene's face talk: u8 kind (2 ped, 3 player), u16 id, i32 ms
                              // (0: it stops)
    G_CAR_DOOR = 167,         // reliable, host -> partner: the story mission's door command on a car: u16 car, u16 opcode
                              // (OPEN_CAR_DOOR, CLOSE_ALL_CAR_DOORS, POP_CAR_DOOR, FIX_CAR_DOOR, OPEN_CAR_DOOR_A_BIT),
                              // i32 door, i32 its other parameter (the bits of a float for OPEN_CAR_DOOR_A_BIT)
    G_NOISE = 168,            // reliable, partner -> host: its own noise meter of "Home Invasion" is full
    G_MISSION_HINT = 169,     // reliable, host -> partner: u8 1 show (0 take away) a help of the host's mission meant for the
                              // partner's own doing, str its GXT key (the crate's load at the van)
    G_MISSION_LOOK_AT = 170,  // reliable, host -> partner: a scene's head turn (TASK_LOOK_AT_CHAR): u8 kind (2 ped, 3 player),
                              // u16 id of who looks, the same of whom, i32 ms
    G_PED_NO_COLLIDE = 171,   // reliable, host -> partner: SET_CHAR_USES_COLLISION_CLOSEST_OBJECT_OF_TYPE of the story mission
                              // on a shared person: u8 kind, u16 id, vec3 at, f32 radius, i32 model, u8 state
    G_PED_ATTACH = 172,       // reliable, host -> partner: the story mission puts a shared person on a car (ATTACH_CHAR_TO_CAR):
                              // u8 1 on (0 off), u8 kind, u16 id, u16 car, vec3 offset, i32 heading, f32 range, i32 weapon
    G_MARK_COLOUR = 173,      // reliable: str the colour the sender chose for its own mark on the others' radar (white, red,
                              // green, blue, yellow, RRGGBB; empty: the allies' blue), on a change and every 15 s
    G_CONTEST = 174,          // reliable: a side activity as a contest: u8 op (1 call, 2 accept, 3 decline, 4 state, 5
                              // cancel), u8 to (a player, 255 all), u8 activity, i32 variant (the race), the sender's run: u8
                              // state (0 not started, 1 playing, 2 done), u8 ok, i32 value, i32 place; u8 why (decline, cancel)
    G_POOL = 175,             // pool for two (POOL2's two-player mode across games, Pool.cpp): u8 op, u8 to; 1 start, 2 end,
                              // 3 fail u8 (0 the owner's game started alone, 1 it left the table), 7 result u8 (0 owner won,
                              // 1 the second player): reliable, each with its u8. 4 view (owner: camera vec3 src, vec3 front,
                              // f32 fov; the opponent's place vec3, f32 heading, i16 block, i16 anim, f32 time, f32 speed, u16
                              // flags; u8 turn, i8 area; the opponent's cue's own animation: i16 block, i16 anim, f32 time,
                              // u16 flags), 5 objects (u8 n, n x u8 slot, u16 model, vec3, quat, u8 look: 1 shown, 2 the
                              // opponent's cue, in the second player's hand), 6 input (second player: its pad's 20 values i16, u8
                              // joypad, f32 mouse x, y), 8 table (to 255: any game with a POOL2 of its own, u16 model, vec3,
                              // quat: the others' table stands so): unreliable
    G_TP_ASK = 176,           // reliable: /tp and /tphere as requests (Teleport.cpp): u8 op (1 may I come over, 2 come over to
                              // me, 3 yes, 4 no), u8 to. The asker's game carries a yes out (/tphere: G_TELEPORT)
    G_MONEY = 177,            // reliable: the players' money (F2, Money.cpp): u8 op; 1 what the sender has now, i32; 2 a gift,
                              // u8 to, i32; 3 the gift came, u8 to (its giver), i32; 4 host -> partner: its share of what
                              // Big Smoke's courier paid the host (Courier.cpp), u8 to, i32
    G_MISSION_LOOK = 164,     // reliable, host -> partner: what the story mission put on its player, worn over the
                              // partner's own look: u8 n, n x (u8 key index 0..27 of CPedClothesDesc, u32 key); 0: off
    G_OBJ_HOLD = 163,         // reliable: the partner holds the copy of a host's mission object: u8 op (1 hold, 2 release,
                              // 3 enter pressed, 4 deny: host -> partner), u16 object, release: vec3 pos, quat rot

    G_VOICE = 200,            // unreliable: voice frame
    G_LAST = 255,
};

inline bool IsRelayed(uint8_t id) { return id >= G_FIRST; }

enum ClientStatus : uint8_t {
    STATUS_MENU = 0,
    STATUS_LOADING = 1,
    STATUS_INGAME = 2,
    STATUS_PAUSED = 3,
};

enum LeaveReason : uint8_t { LEAVE_QUIT = 0, LEAVE_TIMEOUT = 1, LEAVE_KICKED = 2 };

struct Hello {
    uint32_t protocol = 0;
    std::string version;
    std::string name;
    std::string password;
    bool wantsHost = false;
    uint32_t exeSize = 0;

    void Write(Writer& w) const {
        w.U32(protocol);
        w.Str(version, 32);
        w.Str(name, 64);
        w.Str(password, 64);
        w.Bool(wantsHost);
        w.U32(exeSize);
    }
    bool Read(Reader& r) {
        protocol = r.U32();
        version = r.Str(32);
        name = r.Str(64);
        password = r.Str(64);
        wantsHost = r.Bool();
        exeSize = r.U32();
        return r.Ok();
    }
};

struct PlayerEntry {
    uint8_t id = 0;
    std::string name;
    bool isHost = false;
};

struct Welcome {
    uint8_t yourId = 0;
    uint8_t hostId = 0;
    std::string serverName;
    std::string motd, motdEn;
    bool worldAvailable = false;
    bool friendlyFire = false;  // the players can hurt each other (the host chose it creating the game)
    bool partnerTeleport = true;  // the partners may use /tp and /tphere (the host's choice too)
    std::vector<PlayerEntry> players;  // already connected players (excluding you)

    void Write(Writer& w) const {
        w.U8(yourId);
        w.U8(hostId);
        w.Str(serverName, 64);
        w.Str(motd, 512);
        w.Bool(worldAvailable);
        w.U8((uint8_t)players.size());
        for (auto& p : players) {
            w.U8(p.id);
            w.Str(p.name, 64);
            w.Bool(p.isHost);
        }
        w.Str(motdEn, 512);
        w.Bool(friendlyFire);
        w.Bool(partnerTeleport);
    }
    bool Read(Reader& r) {
        yourId = r.U8();
        hostId = r.U8();
        serverName = r.Str(64);
        motd = r.Str(512);
        worldAvailable = r.Bool();
        uint8_t n = r.U8();
        players.clear();
        for (uint8_t i = 0; i < n && r.Ok(); ++i) {
            PlayerEntry p;
            p.id = r.U8();
            p.name = r.Str(64);
            p.isHost = r.Bool();
            players.push_back(p);
        }
        motdEn = r.Str(512);
        friendlyFire = r.Bool();
        partnerTeleport = r.Bool();
        return r.Ok();
    }
};

}  // namespace coop::proto

#pragma once
#include <cmath>
#include <cstdint>

#include "common/lib/buffer/Buffer.h"
#include "shared/game/Game.h"

namespace coop {

// Orientation as a quaternion (compressed to 4 x int16 on the wire).
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
    static Quat FromMatrix(const game::CMatrix& m);
    void ToMatrix(game::CMatrix& m) const;  // writes right/forward/up, keeps pos
    static Quat Slerp(const Quat& a, const Quat& b, float t);
    // q turned on for t seconds by the world angular velocity w (rad/s): a spin goes on from a state as its position
    // does (q maps the vehicle's axes to the world, so the world turn comes first in the product)
    static Quat Turned(const Quat& q, const game::CVector& w, float t) {
        float rate = std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z), a = rate * t;
        if (a < 1e-4f) return q;
        float s = std::sin(a * 0.5f) / rate, c = std::cos(a * 0.5f);
        Quat d{w.x * s, w.y * s, w.z * s, c};
        return Quat{d.w * q.x + d.x * q.w + d.y * q.z - d.z * q.y, d.w * q.y - d.x * q.z + d.y * q.w + d.z * q.x,
                    d.w * q.z + d.x * q.y - d.y * q.x + d.z * q.w, d.w * q.w - d.x * q.x - d.y * q.y - d.z * q.z};
    }
    void Write(Writer& w) const;
    void Read(Reader& r);
};

struct VehicleState {
    uint32_t time = 0;
    game::CVector pos;
    Quat rot;
    game::CVector vel;   // m/s
    game::CVector turn;  // rad/s
    float steer = 0;     // radians
    float gas = 0;       // -1..1
    float brake = 0;     // 0..1
    float health = 1000;
    uint8_t flags = 0;   // F_*

    // F_SPRINT: a bicycle's rider pedals standing (CBmx +0x818 over 5: the BMX_sprint animation)
    enum : uint8_t { F_ENGINE = 1, F_LIGHTS = 2, F_SIREN = 4, F_HORN = 8, F_HANDBRAKE = 16, F_WRECKED = 32, F_SPRINT = 64 };
    // parts the driver moves; X_* says which this vehicle has
    uint8_t extras = 0;
    uint16_t misc = 0;  // X_MISC, CAutomobile::m_wMiscComponentAngle: Hydra nozzles, raised hydraulics, tow hoist, forks
    float gear = 0;     // X_GEAR, CPlane::m_fLandingGearStatus: 0 down, 1 up, negative while going down
    float turretYaw = 0, turretPitch = 0;  // X_TURRET: Rhino, SWAT van, fire truck
    game::CVector lightTarget;             // X_LIGHT: a helicopter's searchlight, CHeli::m_vecSearchLightTarget
    float lightPower = 0;                  // and CHeli::m_fSearchLightIntensity (0..1)
    uint16_t tower = 0;                    // X_TOW: net id of the vehicle towing this one (CVehicle::m_pTowingVehicle)
    float lean = 0, leanFwd = 0;           // X_BIKE: CBike's lean (+0x648) and its rider's weight forward (+0x650)
    enum : uint8_t { X_MISC = 1, X_GEAR = 2, X_TURRET = 4, X_LIGHT = 8, X_TOW = 16, X_BIKE = 32 };

    static VehicleState Capture(game::CVehicle* v);
    void Write(Writer& w) const;
    bool Read(Reader& r);
};

// Static description used to create the vehicle on the other side.
struct VehicleSpawn {
    uint16_t netId = 0;
    uint16_t model = 0;
    uint8_t owner = 0;
    game::CVector pos;
    Quat rot;
    uint8_t colors[4] = {};
    float health = 1000;
    int8_t area = 0;
    uint8_t lockState = 0;
    bool script = false;  // a script's car in its owner's game (a mission car): never an ordinary car elsewhere
    int8_t extras[2] = {-1, -1};  // its components (CVehicle::m_anExtras: a Picador's surfboard, a convertible's roof)

    void Write(Writer& w) const;
    bool Read(Reader& r);
};

}  // namespace coop

#include "entities/vehicle/VehicleState.h"

#include <algorithm>
#include <cmath>

namespace coop {

using game::CMatrix;
using game::CVector;

Quat Quat::FromMatrix(const CMatrix& m) {
    // columns: right (X), forward (Y), up (Z)
    float m00 = m.right.x, m01 = m.forward.x, m02 = m.up.x;
    float m10 = m.right.y, m11 = m.forward.y, m12 = m.up.y;
    float m20 = m.right.z, m21 = m.forward.z, m22 = m.up.z;
    Quat q;
    float tr = m00 + m11 + m22;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 1e-6f) {
        q.x /= n;
        q.y /= n;
        q.z /= n;
        q.w /= n;
    }
    return q;
}

void Quat::ToMatrix(CMatrix& m) const {
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    m.right = {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy)};
    m.forward = {2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx)};
    m.up = {2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy)};
}

Quat Quat::Slerp(const Quat& a, const Quat& bIn, float t) {
    Quat b = bIn;
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) {
        d = -d;
        b.x = -b.x;
        b.y = -b.y;
        b.z = -b.z;
        b.w = -b.w;
    }
    Quat r;
    if (d > 0.9995f) {
        r.x = a.x + (b.x - a.x) * t;
        r.y = a.y + (b.y - a.y) * t;
        r.z = a.z + (b.z - a.z) * t;
        r.w = a.w + (b.w - a.w) * t;
    } else {
        float th = std::acos(d), s = std::sin(th);
        float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
        r.x = a.x * wa + b.x * wb;
        r.y = a.y * wa + b.y * wb;
        r.z = a.z * wa + b.z * wb;
        r.w = a.w * wa + b.w * wb;
    }
    float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    r.x /= n;
    r.y /= n;
    r.z /= n;
    r.w /= n;
    return r;
}

void Quat::Write(Writer& out) const {
    out.I16(Writer::Quant(x, 1.0f));
    out.I16(Writer::Quant(y, 1.0f));
    out.I16(Writer::Quant(z, 1.0f));
    out.I16(Writer::Quant(this->w, 1.0f));
}

void Quat::Read(Reader& r) {
    x = r.I16() / 32767.0f;
    y = r.I16() / 32767.0f;
    z = r.I16() / 32767.0f;
    w = r.I16() / 32767.0f;
    float n = std::sqrt(x * x + y * y + z * z + w * w);
    if (n < 1e-4f) {
        x = y = z = 0;
        w = 1;
    } else {
        x /= n;
        y /= n;
        z /= n;
        w /= n;
    }
}

static Vec3 V(const CVector& v) { return {v.x, v.y, v.z}; }
static CVector G(const Vec3& v) { return {v.x, v.y, v.z}; }

VehicleState VehicleState::Capture(game::CVehicle* v) {
    VehicleState s;
    s.pos = v->GetPosition();
    if (game::CMatrix* m = v->Matrix()) s.rot = Quat::FromMatrix(*m);
    s.vel = v->MoveSpeed() * 50.0f;
    s.turn = v->TurnSpeed() * 50.0f;
    s.steer = v->SteerAngle();
    s.gas = v->GasPedal();
    s.brake = v->BrakePedal();
    s.health = v->Health();
    // the horn: our player's held button (CPad::GetHorn), a driver of the game's own a honk (CVehicle::m_HornCounter)
    game::CPlayerPed* me = game::FindPlayerPed(0);
    bool horn = me && v->Driver() == me ? game::CallMethod<0x53FEE0, bool>(game::GetPad(0)) : game::Field<uint32_t>(v, 0x514) > 0;
    s.flags = (v->EngineOn() ? F_ENGINE : 0) | (v->LightsOn() ? F_LIGHTS : 0) | (v->SirenOn() ? F_SIREN : 0) |
              (horn ? F_HORN : 0) | (v->Status() == 5 ? F_WRECKED : 0) | (v->VehicleFlagsLo() & 0x20 ? F_HANDBRAKE : 0);
    int type = v->VehicleSubClass();
    if (type <= game::VEHICLE_TYPE_PLANE || type == game::VEHICLE_TYPE_TRAILER) {  // CAutomobile and its subclasses
        s.extras |= X_MISC;
        s.misc = game::Field<uint16_t>(v, 0x86C);
    }
    if (type == game::VEHICLE_TYPE_PLANE) {
        s.extras |= X_GEAR;
        s.gear = game::Field<float>(v, 0x9CC);
    }
    int model = v->ModelIndex();
    if (model == 432 || model == 601 || model == 407) {
        s.extras |= X_TURRET;
        s.turretYaw = game::Field<float>(v, 0x94C);   // CAutomobile::m_fDoomVerticalRotation
        s.turretPitch = game::Field<float>(v, 0x950);  // m_fDoomHorizontalRotation
    }
    // CBike::ProcessControl works the lean out of the step's sideways speed change (0x6BBA69): a copy's speed is set
    // from every state, and it rocked from side to side with its rider's weight never forward or back
    if (type == game::VEHICLE_TYPE_BIKE || type == game::VEHICLE_TYPE_BMX) {
        s.extras |= X_BIKE;
        s.lean = game::Field<float>(v, 0x648);     // m_RideAnimData.LeanAngle
        s.leanFwd = game::Field<float>(v, 0x650);  // LeanFwd
        // the rider's quick taps on the pedals: only a player's bike counts them (CBmx::ProcessAI, 0x6C14B2), and
        // CBmx::ProcessDrivingAnims stands its rider up on the pedals by this alone
        if (type == game::VEHICLE_TYPE_BMX && game::Field<float>(v, 0x818) > 5.0f) s.flags |= F_SPRINT;
    }
    // CHeli::ProcessControl (0x6C7050) lights it for the police AI or a player's Police Maverick (the horn)
    if (type == game::VEHICLE_TYPE_HELI && game::Field<uint8_t>(v, 0xA11) && game::Field<float>(v, 0xA00) > 0.0f) {
        s.extras |= X_LIGHT;
        s.lightTarget = game::Field<CVector>(v, 0x9F4);
        s.lightPower = game::Field<float>(v, 0xA00);
    }
    return s;
}

void VehicleState::Write(Writer& w) const {
    w.U32(time);
    w.Vec(V(pos));
    rot.Write(w);
    w.VecQ(V(vel), 150.0f);
    w.VecQ(V(turn), 30.0f);
    w.I8((int8_t)std::clamp(steer / 1.0f * 127.0f, -127.0f, 127.0f));
    w.SUnit8(gas);
    w.Unit8(brake);
    w.U16((uint16_t)std::clamp(health, 0.0f, 65535.0f));
    w.U8(flags);
    w.U8(extras);
    if (extras & X_MISC) w.U16(misc);
    if (extras & X_GEAR) w.I16(Writer::Quant(gear, 1.0f));
    if (extras & X_TURRET) {
        w.I16(Writer::Quant(turretYaw, 3.2f));
        w.I16(Writer::Quant(turretPitch, 3.2f));
    }
    if (extras & X_LIGHT) {
        w.Vec(V(lightTarget));
        w.Unit8(lightPower);
    }
    if (extras & X_TOW) w.U16(tower);
    if (extras & X_BIKE) {
        w.I16(Writer::Quant(lean, 3.2f));
        w.I16(Writer::Quant(leanFwd, 3.2f));
    }
}

bool VehicleState::Read(Reader& r) {
    time = r.U32();
    pos = G(r.Vec());
    rot.Read(r);
    vel = G(r.VecQ(150.0f));
    turn = G(r.VecQ(30.0f));
    steer = r.I8() / 127.0f;
    gas = r.SUnit8();
    brake = r.Unit8();
    health = (float)r.U16();
    flags = r.U8();
    extras = r.U8();
    if (extras & X_MISC) misc = r.U16();
    if (extras & X_GEAR) gear = r.I16() / 32767.0f;
    if (extras & X_TURRET) {
        turretYaw = r.I16() / 32767.0f * 3.2f;
        turretPitch = r.I16() / 32767.0f * 3.2f;
    }
    if (extras & X_LIGHT) {
        lightTarget = G(r.Vec());
        lightPower = r.Unit8();
    }
    if (extras & X_TOW) tower = r.U16();
    if (extras & X_BIKE) {
        lean = r.I16() / 32767.0f * 3.2f;
        leanFwd = r.I16() / 32767.0f * 3.2f;
    }
    return r.Ok();
}

void VehicleSpawn::Write(Writer& w) const {
    w.U16(netId);
    w.U16(model);
    w.U8(owner);
    w.Vec(V(pos));
    rot.Write(w);
    w.Raw(colors, 4);
    w.U16((uint16_t)std::clamp(health, 0.0f, 65535.0f));
    w.I8(area);
    w.U8(lockState);
    w.U8(script);
    w.I8(extras[0]);
    w.I8(extras[1]);
}

bool VehicleSpawn::Read(Reader& r) {
    netId = r.U16();
    model = r.U16();
    owner = r.U8();
    pos = G(r.Vec());
    rot.Read(r);
    r.Raw(colors, 4);
    health = (float)r.U16();
    area = r.I8();
    lockState = r.U8();
    script = r.U8() != 0;
    extras[0] = r.I8();
    extras[1] = r.I8();
    return r.Ok();
}

}  // namespace coop

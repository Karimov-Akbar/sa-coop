#include "entities/player/PlayerState.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace coop {

static int8_t Stick(int16_t v) { return (int8_t)std::clamp<int>(v, -128, 127); }
static int16_t Btn(bool b) { return b ? 255 : 0; }

PadState PadState::From(const game::CControllerState& s) {
    PadState p;
    p.lx = Stick(s.LeftStickX);
    p.ly = Stick(s.LeftStickY);
    p.rx = Stick(s.RightStickX);
    p.ry = Stick(s.RightStickY);
    uint16_t b = 0;
    if (s.ButtonSquare) b |= B_SQUARE;
    if (s.ButtonCross) b |= B_CROSS;
    if (s.ButtonCircle) b |= B_CIRCLE;
    if (s.ButtonTriangle) b |= B_TRIANGLE;
    if (s.RightShoulder1) b |= B_RS1;
    if (s.LeftShoulder1) b |= B_LS1;
    if (s.LeftShoulder2) b |= B_LS2;
    if (s.RightShoulder2) b |= B_RS2;
    if (s.ShockButtonL) b |= B_SHOCKL;
    if (s.ShockButtonR) b |= B_SHOCKR;
    if (s.DPadUp) b |= B_DUP;
    if (s.DPadDown) b |= B_DDOWN;
    if (s.DPadLeft) b |= B_DLEFT;
    if (s.DPadRight) b |= B_DRIGHT;
    if (s.m_bPedWalk) b |= B_WALK;
    if (s.Select) b |= B_SELECT;
    p.buttons = b;
    return p;
}

void PadState::To(game::CControllerState& s) const {
    memset(&s, 0, sizeof(s));
    // the game uses +-128 for full deflection
    s.LeftStickX = lx == 127 ? 128 : lx;
    s.LeftStickY = ly == 127 ? 128 : ly;
    s.RightStickX = rx == 127 ? 128 : rx;
    s.RightStickY = ry == 127 ? 128 : ry;
    s.ButtonSquare = Btn(buttons & B_SQUARE);
    s.ButtonCross = Btn(buttons & B_CROSS);
    s.ButtonCircle = Btn(buttons & B_CIRCLE);
    s.ButtonTriangle = Btn(buttons & B_TRIANGLE);
    s.RightShoulder1 = Btn(buttons & B_RS1);
    s.LeftShoulder1 = Btn(buttons & B_LS1);
    s.LeftShoulder2 = Btn(buttons & B_LS2);
    s.RightShoulder2 = Btn(buttons & B_RS2);
    s.ShockButtonL = Btn(buttons & B_SHOCKL);
    s.ShockButtonR = Btn(buttons & B_SHOCKR);
    s.DPadUp = Btn(buttons & B_DUP);
    s.DPadDown = Btn(buttons & B_DDOWN);
    s.DPadLeft = Btn(buttons & B_DLEFT);
    s.DPadRight = Btn(buttons & B_DRIGHT);
    s.m_bPedWalk = Btn(buttons & B_WALK);
    s.Select = Btn(buttons & B_SELECT);
}

void PadState::Write(Writer& w) const {
    w.I8(lx);
    w.I8(ly);
    w.I8(rx);
    w.I8(ry);
    w.U16(buttons);
}

void PadState::Read(Reader& r) {
    lx = r.I8();
    ly = r.I8();
    rx = r.I8();
    ry = r.I8();
    buttons = r.U16();
}

static Vec3 V(const game::CVector& v) { return {v.x, v.y, v.z}; }
static game::CVector G(const Vec3& v) { return {v.x, v.y, v.z}; }

void CameraState::Write(Writer& w) const {
    w.U16(mode);
    w.U16(weaponMode);
    w.U8((uint8_t)std::clamp(fov, 0.0f, 255.0f));
    w.VecQ(V(front), 1.0f);
    w.VecQ(V(sourceOffset), 64.0f);
    w.VecQ(V(up), 1.0f);
    w.Angle(orientation);
    w.Angle(alpha);
    w.Angle(beta);
    w.I16(Writer::Quant(lookPitch, 3.2f));
    w.U8((useMouse3rd ? 1 : 0) | (lookLeft ? 2 : 0) | (lookRight ? 4 : 0) | (driveByOff ? 8 : 0));
    w.Unit8(chairX);
    w.Unit8(chairY);
}

void CameraState::Read(Reader& r) {
    mode = r.U16();
    weaponMode = r.U16();
    fov = (float)r.U8();
    front = G(r.VecQ(1.0f));
    sourceOffset = G(r.VecQ(64.0f));
    up = G(r.VecQ(1.0f));
    orientation = game::WrapAngle(r.Angle());
    alpha = game::WrapAngle(r.Angle());
    beta = game::WrapAngle(r.Angle());
    lookPitch = r.I16() / 32767.0f * 3.2f;
    uint8_t bits = r.U8();
    useMouse3rd = bits & 1, lookLeft = bits & 2, lookRight = bits & 4, driveByOff = bits & 8;
    chairX = r.Unit8();
    chairY = r.Unit8();
}

void OnFootState::Write(Writer& w) const {
    w.U32(time);
    w.Vec(V(pos));
    w.VecQ(V(vel), 40.0f);
    w.Angle(rotation);
    w.Angle(aimRotation);
    w.U8(health);
    w.U8(armour);
    w.U8(weapon);
    w.U16(ammo);
    w.U8(flags);
    w.U8(flags2);
    w.I8(area);
    w.U8(moveState);
    w.U8(wanted);
    pad.Write(w);
    cam.Write(w);
}

bool OnFootState::Read(Reader& r) {
    time = r.U32();
    pos = G(r.Vec());
    vel = G(r.VecQ(40.0f));
    rotation = game::WrapAngle(r.Angle());
    aimRotation = game::WrapAngle(r.Angle());
    health = r.U8();
    armour = r.U8();
    weapon = r.U8();
    ammo = r.U16();
    flags = r.U8();
    flags2 = r.U8();
    area = r.I8();
    moveState = r.U8();
    wanted = r.U8();
    pad.Read(r);
    cam.Read(r);
    return r.Ok();
}

}  // namespace coop

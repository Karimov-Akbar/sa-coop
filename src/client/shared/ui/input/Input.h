#pragma once
#include <cstdint>
#include <vector>

namespace coop::ui {

// Window-procedure level input. The game reads the keyboard only through window messages
// (WM_KEYDOWN/UP) and the mouse through DirectInput, so:
//  * keyboard capture = swallowing the messages;
//  * mouse capture    = skipping CPad::UpdateMouse (hooked in Patches) and not re-centring the cursor.
struct InputState {
    // per-frame event queues (filled by the window procedure, consumed by the UI each frame)
    std::vector<int> keyDowns;   // virtual keys (incl. auto-repeat)
    std::vector<wchar_t> chars;  // typed characters
    float mouseX = 0, mouseY = 0;  // in back-buffer pixels
    bool lmb = false;
    bool lmbPressed = false;   // went down this frame
    bool lmbReleased = false;
    int wheel = 0;
    bool ctrl = false, shift = false;
};

class Input {
public:
    void Install(void* hwnd);
    bool Installed() const { return m_hwnd != nullptr; }

    // While set, keyboard messages never reach the game.
    void SetKeyboardCapture(bool on);
    // While set, the game does not receive mouse input and the cursor is free.
    void SetMouseCapture(bool on);
    // While set, the game's own mouse reading (DirectInput, CPad::UpdateMouse) goes on, but its movement is handed to
    // us (TakeMouseMotion) instead of the camera and its buttons are dropped: an in-game overlay with a cursor of its
    // own (the emote wheel). The OS cursor stays the game's. Not reading the device would pile the motion up there for
    // the camera to take in one go afterwards.
    void SetMouseRelative(bool on) {
        m_mouseRelative = on;
        m_motionX = m_motionY = 0;
    }
    bool KeyboardCaptured() const { return m_kbCapture; }
    bool MouseCaptured() const { return m_mouseCapture; }
    bool MouseRelative() const { return m_mouseRelative; }
    void AddMouseMotion(float dx, float dy) {  // DirectInput counts, x right, y down
        m_motionX += dx;
        m_motionY += dy;
    }
    void TakeMouseMotion(float& dx, float& dy) {  // since the last call
        dx = m_motionX;
        dy = m_motionY;
        m_motionX = m_motionY = 0;
    }

    InputState& State() { return m_state; }
    void EndFrame();  // clears per-frame events
    void UpdateMouse(float backBufferW, float backBufferH);

    void* Hwnd() const { return m_hwnd; }
    // Called by the window procedure. Returns true if the message must not reach the game.
    bool OnMessage(unsigned msg, uintptr_t wParam, intptr_t lParam);

    // Clears the game's keyboard state so no key stays "held" after capture starts.
    static void ClearGameKeys();

private:
    void* m_hwnd = nullptr;
    bool m_kbCapture = false;
    bool m_mouseCapture = false;
    bool m_mouseRelative = false;
    float m_motionX = 0, m_motionY = 0;
    InputState m_state;
};

Input& In();

}  // namespace coop::ui

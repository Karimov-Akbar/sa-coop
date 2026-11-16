#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace coop::ui {

// The keys under a call in the middle of the screen: Y and N, or right and left on the D-pad with the game set to a pad
// (they are its own yes and no to people in the street, and answer the calls too: core::PadAnswers). No `no`: Y alone
std::string AnswerHint(const char* yes, const char* no = nullptr);

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
    bool KeyboardCaptured() const { return m_kbCapture; }
    bool MouseCaptured() const { return m_mouseCapture; }
    // Captured while the game holds the mouse exclusively (its fullscreen play): our cursor moves by the game's own
    // reading of the mouse, fed here from CPad::UpdateMouse (Patches)
    bool CursorFromGameMouse() const;
    void FeedGameMouse(float dx, float dy, bool lmb, int wheel);

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
    bool m_fromGame = false;          // the cursor below is ours (CursorFromGameMouse)
    float m_gameX = 0, m_gameY = 0;  // in client pixels
    bool m_gameLmb = false;
    InputState m_state;
};

Input& In();

}  // namespace coop::ui

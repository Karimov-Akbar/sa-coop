// Voice chat: push-to-talk (or always on) capture with WinMM, 16 kHz mono, 20 ms frames, IMA ADPCM
// (~68 kbit/s), sent unreliable on the voice channel. Every remote player gets its own waveOut stream
// with a small jitter buffer; the volume fades with distance down to a "radio" floor.
// WinMM runs without callbacks, polled every 5 ms by a thread of its own (8 x 20 ms of capture headroom). A WinMM call
// blocks for a second now and then (waveInAddBuffer, on a player's machine with nobody talking): polled on the game's
// thread, the whole game stood still that long, the lags of 1-2 s players reported. The game's thread only encodes,
// sends and queues what it heard.
#include "features/voice/Voice.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/ini/Ini.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/player/Players.h"
#include "features/voice/Adpcm.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop {

namespace {
constexpr int kRate = 16000, kFrame = 320;  // 20 ms
constexpr int kInBuffers = 8, kOutBuffers = 16;
constexpr size_t kMaxQueue = 12;  // 240 ms: older audio is dropped
constexpr size_t kStartQueue = 3;  // 60 ms of buffering before a stream starts
using Frame = std::array<int16_t, kFrame>;
// the microphone's icon (ui/microphone.svg, in coop.rc as MICROPHONE): a character of the fonts (ui::kMicIcon)
using ui::kMicIcon;
using ui::kMicIconUtf8;

WAVEFORMATEX Format16k() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 1;
    f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 2;
    f.nAvgBytesPerSec = kRate * 2;
    return f;
}

// a remote player's playback device: the audio thread's alone
struct Output {
    HWAVEOUT out = nullptr;
    WAVEHDR hdr[kOutBuffers] = {};
    int16_t buf[kOutBuffers][kFrame] = {};
    bool playing = false;

    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    ~Output() {
        if (!out) return;
        waveOutReset(out);
        for (WAVEHDR& h : hdr)
            if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(out, &h, sizeof(h));
        waveOutClose(out);
    }
};

// what the game's thread heard from a remote player, for the audio thread to play
struct Incoming {
    std::deque<Frame> queue;
    bool gone = false;  // the player left: its device closes
};

// a remote player's stream as the game's thread follows it
struct Heard {
    uint16_t lastSeq = 0;
    bool hasSeq = false;
    uint64_t lastHeardMs = 0;
};

class Voice : public Module {
public:
    const char* Name() const override { return "Voice"; }

    void OnInstallHooks() override {
        Net().SetHandler(proto::G_VOICE, [](uint8_t from, Reader& r) { Instance().Handle(from, r); });
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&kMicIcon, &self);
        if (HRSRC res = self ? FindResourceW(self, L"MICROPHONE", (LPCWSTR)RT_RCDATA) : nullptr)
            if (HGLOBAL data = LoadResource(self, res))
                m_micIcon = ui::Gfx().SetIcon(kMicIcon, std::string_view((const char*)LockResource(data), SizeofResource(self, res)));
        if (!m_micIcon) LOGW("Voice: the microphone's icon could not be read: a dot instead");
        Ini ini;  // test setups: a 440 Hz tone instead of the microphone (no device needed), transmitted all the time
        ini.Load(Cfg().iniPath);
        m_testTone = ini.GetBool("debug", "voice_test_tone", false);
    }

    void OnSessionStart() override {
        m_micWarned = m_silenceWarned = false;
        if (Cfg().voiceEnabled && !m_testTone) {
            m_wantMic = true;
            StartAudioThread();
        }
    }
    void OnSessionEnd() override {
        m_wantMic = false;
        m_transmitting = false;
        m_heard.clear();
        std::lock_guard<std::mutex> lock(m_mx);
        m_captured.clear();
        for (auto& [id, in] : m_incoming) in.gone = true;
    }
    void OnPeerLeave(uint8_t id) override {
        m_heard.erase(id);
        std::lock_guard<std::mutex> lock(m_mx);
        if (auto it = m_incoming.find(id); it != m_incoming.end()) it->second.gone = true;
    }

    void OnPostProcess() override {
        if (!Net().Online()) return;
        if (m_audioOk < 0 && !m_audioOffTold) {
            m_audioOffTold = true;
            ui::TheChat().AddError(TR("Голосовой чат выключен: звуковой драйвер Windows падает при открытии микрофона. Обновите драйвер звука.",
                                      "Voice chat is off: the Windows sound driver crashes when the microphone opens. Update the sound driver."));
        }
        PumpCapture();
        if (m_rxFrames && NowMs() - m_rxLogMs > 5000) {
            m_rxLogMs = NowMs();
            LOGI("Voice: %d frames received, rms %.0f", m_rxFrames, std::sqrt(m_rxEnergy / (m_rxFrames * (double)kFrame)));
            m_rxFrames = 0;
            m_rxEnergy = 0;
        }
    }

    void OnRender() override {
        if (!core::IsPlayerReady()) return;
        auto& g = ui::Gfx();
        float s = g.Scale(), y = g.Height() * 0.62f;
        if (m_transmitting) {
            g.Text(ui::FONT_SMALL, 24 * s, y, Format("{FF5050}%s{FFFFFF} %s", m_micIcon ? kMicIconUtf8 : "●", TR("Микрофон", "Mic")),
                   0xFFFFFFFF, ui::TEXT_OUTLINE);
            y += g.LineHeight(ui::FONT_SMALL);
        }
        for (auto& [id, heard] : m_heard)  // who is heard now, wherever they are (the name tag needs them in sight)
            if (Speaking(id)) {
                g.Text(ui::FONT_SMALL, 24 * s, y, ui::TalkingMark() + Net().PeerName(id), 0xFFFFFFFF, ui::TEXT_OUTLINE);
                y += g.LineHeight(ui::FONT_SMALL);
            }
    }

    bool Speaking(uint8_t id) const {
        auto it = m_heard.find(id);
        return it != m_heard.end() && NowMs() - it->second.lastHeardMs < 300;
    }
    bool Transmitting() const { return m_transmitting; }

    static Voice& Instance() {
        static Voice v;
        return v;
    }

private:
    void StartAudioThread() {
        if (m_threadStarted) return;
        m_threadStarted = true;
        std::thread([this] {
            m_audioOk = ProbeAudio() ? 1 : -1;
            while (m_audioOk > 0) {
                Sleep(5);
                CaptureWork();
                PlaybackWork();
            }
        }).detach();
    }

    // The first WinMM call loads Windows' audio drivers into the game (winmmbase.dll, wdmaud.drv): on some machines they
    // crash there, and the game went down the moment its player joined (#80, 0.2.7 the same). coop-server.exe makes the
    // same calls first in a process of its own: its crash keeps the voice off here. A check that cannot run or hangs
    // changes nothing (a hang would stall this thread only).
    static bool ProbeAudio() {
        std::wstring exe = Cfg().coopDir + L"coop-server.exe";
        std::wstring cmd = L"\"" + exe + L"\" --probe-audio";
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            LOGW("Voice: the sound check did not start (error %lu)", GetLastError());
            return true;
        }
        DWORD wait = WaitForSingleObject(pi.hProcess, 8000), code = 0;
        if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 0);
        else GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code < 0xC0000000) return true;
        LOGW("Voice: Windows' sound drivers crashed in the sound check (%08lX): the voice chat stays off", code);
        return false;
    }

    // ---- the audio thread
    bool OpenMic() {
        WAVEFORMATEX f = Format16k();
        UINT dev = Cfg().micDevice < 0 ? WAVE_MAPPER : (UINT)Cfg().micDevice;
        MMRESULT r = waveInOpen(&m_hin, dev, &f, 0, 0, CALLBACK_NULL);
        if (r != MMSYSERR_NOERROR) {
            m_hin = nullptr;
            LOGW("Voice: no microphone available (waveInOpen failed: %u)", r);
            return false;
        }
        for (int i = 0; i < kInBuffers; ++i) {
            m_inHdr[i] = {};
            m_inHdr[i].lpData = (LPSTR)m_inBuf[i];
            m_inHdr[i].dwBufferLength = sizeof(m_inBuf[i]);
            waveInPrepareHeader(m_hin, &m_inHdr[i], sizeof(WAVEHDR));
            waveInAddBuffer(m_hin, &m_inHdr[i], sizeof(WAVEHDR));
        }
        m_nextIn = 0;
        if ((r = waveInStart(m_hin)) != MMSYSERR_NOERROR) {
            LOGW("Voice: the microphone does not start (waveInStart failed: %u)", r);
            CloseMic();
            return false;
        }
        LOGI("Voice: microphone open (push to talk: key %02X%s)", Cfg().voiceKey, Cfg().voiceAlwaysOn ? ", always on" : "");
        return true;
    }

    void CloseMic() {
        if (!m_hin) return;
        waveInReset(m_hin);
        for (WAVEHDR& h : m_inHdr)
            if (h.dwFlags & WHDR_PREPARED) waveInUnprepareHeader(m_hin, &h, sizeof(h));
        waveInClose(m_hin);
        m_hin = nullptr;
    }

    void CaptureWork() {
        if (!m_wantMic) {
            CloseMic();
            m_micTriedMs = 0;
            return;
        }
        if (!m_hin) {
            // at the session's start, then again every 3 s while the key is held: the microphone may be plugged in or
            // allowed later
            if (m_micTriedMs && (!m_keyHeld || NowMs() - m_micTriedMs < 3000)) return;
            bool retry = m_micTriedMs != 0;
            m_micTriedMs = NowMs();
            if (!OpenMic() && retry) m_micFailed = true;  // the game's thread tells the player
            return;
        }
        // in the order they were queued: by index, a wrap-around would swap 20 ms pieces
        for (int n = 0; n < kInBuffers; ++n) {
            WAVEHDR& h = m_inHdr[m_nextIn];
            if (!(h.dwFlags & WHDR_DONE)) break;
            {
                std::lock_guard<std::mutex> lock(m_mx);
                if (h.dwBytesRecorded == sizeof(m_inBuf[m_nextIn])) {
                    m_captured.emplace_back();
                    memcpy(m_captured.back().data(), m_inBuf[m_nextIn], sizeof(m_inBuf[m_nextIn]));
                    while (m_captured.size() > (size_t)kInBuffers) m_captured.pop_front();
                } else {
                    m_captureGap = true;
                }
            }
            h.dwFlags &= ~WHDR_DONE;
            h.dwBytesRecorded = 0;
            waveInAddBuffer(m_hin, &h, sizeof(WAVEHDR));
            m_nextIn = (m_nextIn + 1) % kInBuffers;
        }
    }

    void PlaybackWork() {
        std::vector<uint8_t> ids;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            for (auto& [id, in] : m_incoming) ids.push_back(id);
        }
        for (uint8_t id : ids) {
            Output& o = m_outputs[id];
            int free = 0;
            if (o.out)
                for (WAVEHDR& h : o.hdr) free += (h.dwFlags & WHDR_DONE) ? 1 : 0;
            std::vector<Frame> take;
            bool gone = false, more = false;
            {
                std::lock_guard<std::mutex> lock(m_mx);
                auto in = m_incoming.find(id);
                if (in == m_incoming.end() || in->second.gone) {
                    gone = true;
                    if (in != m_incoming.end()) m_incoming.erase(in);
                } else {
                    auto& q = in->second.queue;
                    if (o.playing || q.size() >= kStartQueue) {
                        size_t n = std::min(q.size(), o.out ? (size_t)free : (size_t)kOutBuffers);
                        for (size_t i = 0; i < n; ++i) take.push_back(q.front()), q.pop_front();
                    }
                    more = !q.empty();
                }
            }
            if (gone) {
                m_outputs.erase(id);  // ~Output closes its device
                continue;
            }
            if (!o.out) {
                if (take.empty()) continue;
                WAVEFORMATEX f = Format16k();
                UINT dev = Cfg().speakerDevice < 0 ? WAVE_MAPPER : (UINT)Cfg().speakerDevice;
                if (waveOutOpen(&o.out, dev, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
                    o.out = nullptr;
                    continue;  // what came is dropped, the next try comes with the next talk
                }
                for (WAVEHDR& h : o.hdr) h.dwFlags = WHDR_DONE;  // all free
            }
            bool any = false;
            size_t next = 0;
            for (int i = 0; i < kOutBuffers; ++i) {
                WAVEHDR& h = o.hdr[i];
                if (!(h.dwFlags & WHDR_DONE)) {
                    any = true;
                    continue;
                }
                if (next >= take.size()) continue;
                if (h.dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(o.out, &h, sizeof(h));
                memcpy(o.buf[i], take[next++].data(), sizeof(o.buf[i]));
                h = {};
                h.lpData = (LPSTR)o.buf[i];
                h.dwBufferLength = sizeof(o.buf[i]);
                waveOutPrepareHeader(o.out, &h, sizeof(h));
                waveOutWrite(o.out, &h, sizeof(h));
                any = true;
            }
            o.playing = any || more;  // re-buffer after a gap
        }
    }

    // ---- the game's thread
    bool PushToTalk() const {
        if (Cfg().voiceAlwaysOn || m_testTone) return true;
        // only while our window is active and nobody types in the chat
        return game::IsForeground() && !ui::TheChat().IsOpen() && (GetAsyncKeyState(Cfg().voiceKey) & 0x8000);
    }

    void PumpCapture() {
        m_transmitting = Cfg().voiceEnabled && PushToTalk();  // the indicator shows the key even without a microphone
        m_keyHeld = m_transmitting;
        if (m_testTone) {
            uint64_t now = NowMs();
            if (now - m_toneMs > 200) m_toneMs = now;
            for (; now - m_toneMs >= 20; m_toneMs += 20) {  // a frame every 20 ms
                int16_t pcm[kFrame];
                for (int k = 0; k < kFrame; ++k, ++m_tonePhase) pcm[k] = (int16_t)(8000 * std::sin(m_tonePhase * 0.1727876f));
                Send(pcm);
            }
            LogSent();
            return;
        }
        if (m_micFailed.exchange(false) && !m_micWarned) {
            m_micWarned = true;
            ui::TheChat().AddError(TR("Не удалось открыть микрофон. Проверьте, что он подключён, а в Windows разрешён доступ к микрофону для классических приложений.",
                                      "Could not open the microphone. Check that it is plugged in and that Windows lets desktop apps use the microphone."));
        }
        std::deque<Frame> frames;
        bool gap;
        {
            std::lock_guard<std::mutex> lock(m_mx);
            frames.swap(m_captured);
            gap = m_captureGap;
            m_captureGap = false;
        }
        if (gap) m_enc = {};
        for (Frame& f : frames) {
            if (m_transmitting) Send(f.data());
            else m_enc = {};  // a new talk spurt starts from silence
        }
        LogSent();
    }

    void Send(int16_t* pcm) {
        float gain = Cfg().micGain;
        for (int k = 0; k < kFrame; ++k) {
            if (gain != 1.0f) pcm[k] = (int16_t)std::clamp(pcm[k] * gain, -32768.0f, 32767.0f);
            m_txEnergy += (double)pcm[k] * pcm[k];
        }
        ++m_txFrames;
        std::vector<uint8_t> data = adpcm::Encode(pcm, kFrame, m_enc);
        Writer w(proto::G_VOICE);
        w.U16(m_seq++);
        w.Bytes(data.data(), data.size());
        Net().Send(proto::CH_VOICE, w);
    }

    // what the microphone gives: a privacy block in Windows records pure silence
    void LogSent() {
        if (!m_txFrames || NowMs() - m_txLogMs < 5000) return;
        m_txLogMs = NowMs();
        double rms = std::sqrt(m_txEnergy / (m_txFrames * (double)kFrame));
        LOGI("Voice: %d frames sent, rms %.0f", m_txFrames, rms);
        if (rms < 1.0 && m_txFrames >= 100 && !m_silenceWarned) {
            m_silenceWarned = true;
            ui::TheChat().AddError(TR("Микрофон передаёт тишину. Проверьте, что в Windows разрешён доступ к микрофону для классических приложений, а нужный микрофон выбран устройством записи по умолчанию.",
                                      "The microphone sends only silence. Check that Windows lets desktop apps use the microphone and that the right one is the default recording device."));
        }
        m_txFrames = 0;
        m_txEnergy = 0;
    }

    void Handle(uint8_t from, Reader& r) {
        uint16_t seq = r.U16();
        std::vector<uint8_t> data = r.Bytes(1024);
        std::vector<int16_t> pcm;
        if (!r.Ok() || from == Net().MyId() || !Cfg().voiceEnabled || m_audioOk < 0 ||
            !adpcm::Decode(data.data(), data.size(), pcm) || pcm.size() != kFrame)
            return;
        Heard& h = m_heard[from];
        if (h.hasSeq && (int16_t)(seq - h.lastSeq) <= 0) return;  // late or duplicate
        for (int16_t v : pcm) m_rxEnergy += (double)v * v;
        ++m_rxFrames;
        h.lastSeq = seq;
        h.hasSeq = true;
        h.lastHeardMs = NowMs();
        if (RemotePlayer* rp = Players::Instance().Get(from)) rp->heardMs = h.lastHeardMs;  // its name tag
        float vol = Cfg().voiceVolume * Proximity(from);
        Frame f;
        for (int k = 0; k < kFrame; ++k) f[k] = (int16_t)std::clamp(pcm[k] * vol, -32768.0f, 32767.0f);
        StartAudioThread();  // a player who only listens (no microphone of ours wanted)
        std::lock_guard<std::mutex> lock(m_mx);
        Incoming& in = m_incoming[from];
        in.gone = false;
        in.queue.push_back(f);
        while (in.queue.size() > kMaxQueue) in.queue.pop_front();
    }

    // full volume up to 15 m, then down to a radio-like floor at 80 m (co-op partners keep talking)
    static float Proximity(uint8_t id) {
        if (!Cfg().voiceProximity || !core::IsPlayerReady()) return 1.0f;
        const RemotePlayer* rp = Players::Instance().Get(id);
        if (!rp || !rp->hasState) return 1.0f;
        float d = (rp->state.pos - game::FindPlayerPed(0)->GetPosition()).Length();
        return std::clamp(1.0f - (d - 15.0f) / 65.0f, 0.35f, 1.0f);
    }

    // shared by the two threads
    std::mutex m_mx;
    std::deque<Frame> m_captured;  // whole 20 ms frames, oldest first
    bool m_captureGap = false;     // a buffer came back short: the encoder starts over
    std::map<uint8_t, Incoming> m_incoming;
    std::atomic<bool> m_wantMic{false}, m_keyHeld{false}, m_micFailed{false};
    std::atomic<int> m_audioOk{0};  // the sound check: 0 not yet, 1 the devices may be used, -1 they crash
    bool m_micIcon = false;          // the icon is in the fonts
    bool m_audioOffTold = false;
    bool m_threadStarted = false;  // the game's thread's

    // the audio thread's
    HWAVEIN m_hin = nullptr;
    int m_nextIn = 0;  // the capture buffer the device fills first
    uint64_t m_micTriedMs = 0;
    WAVEHDR m_inHdr[kInBuffers] = {};
    int16_t m_inBuf[kInBuffers][kFrame] = {};
    std::map<uint8_t, Output> m_outputs;

    // the game's thread's
    bool m_micWarned = false, m_silenceWarned = false;
    adpcm::State m_enc;
    uint16_t m_seq = 0;
    bool m_transmitting = false;
    std::map<uint8_t, Heard> m_heard;
    bool m_testTone = false;
    unsigned m_tonePhase = 0;
    uint64_t m_toneMs = 0;
    int m_rxFrames = 0, m_txFrames = 0;
    double m_rxEnergy = 0, m_txEnergy = 0;
    uint64_t m_rxLogMs = 0, m_txLogMs = 0;
};

struct Reg {
    Reg() { core::RegisterModule(&Voice::Instance()); }
} g_reg;
}  // namespace

bool VoiceIsSpeaking(uint8_t id) { return Voice::Instance().Speaking(id); }
bool VoiceIsTransmitting() { return Voice::Instance().Transmitting(); }

}  // namespace coop

// ============================================================================
//  love_show.cpp  -  lyric-synced desktop "music video" for Windows
//
//  Put these next to the .exe:
//     song.mp3    - the song
//     lyrics.lrc  - the synced lyrics in standard LRC format ("[mm:ss.xx]text"),
//                   saved as UTF-8. Metadata tags like [ti:...] are ignored.
//                   The lyrics are read at runtime, so nothing is hardcoded.
//
//  Press ESC at any time to stop the show. Everything is restored on exit.
//
//  Build (MSVC):   cl /EHsc /O2 /utf-8 love_show.cpp
//  Build (MinGW):  g++ -O2 -mwindows love_show.cpp -o love_show.exe -lwinmm -lgdi32 -luser32 -ldwmapi
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 13
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr float    kPi       = 3.14159265f;
constexpr int      kFallbackEndMs = 193570;      // used only if the audio length is unknown
constexpr int      kOkId     = 1;
constexpr COLORREF kBg       = RGB(18, 10, 16);
constexpr COLORREF kKey      = RGB(255, 0, 255); // overlay transparency key
constexpr COLORREF kCream    = RGB(247, 244, 222);
constexpr COLORREF kPink     = RGB(255, 170, 190);
constexpr COLORREF kRed      = RGB(255, 85, 115);

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
HWND    g_hwnd = nullptr, g_hStatic = nullptr, g_hBtn = nullptr, g_overlay = nullptr;
HBRUSH  g_bgBrush = nullptr;
HFONT   g_fontText = nullptr, g_fontBtn = nullptr;
int     g_sw = 0, g_sh = 0;                      // screen size (pixels)
int     g_winW = 0, g_winH = 0;

// Timing
Clock::time_point g_t0;
bool g_audioOk = false;
std::wstring g_audioInfo;                        // why audio didn't start (shown in the popup)
int  g_endMs = kFallbackEndMs;

// Popup motion
float g_winX = 0, g_winY = 0;                    // base position of popup
int   g_lastX = -99999, g_lastY = -99999;
bool  g_follow = false, g_dodge = false;
struct ShakeState { int start; int dur; float amp; };
ShakeState g_shake = {0, 0, 0.f};
float g_shakeX = 0, g_shakeY = 0;
int   g_lastShakeMs = 0;

// Lyrics (parsed from the .lrc file, shown with a typewriter reveal)
struct LrcLine { int ms; std::wstring text; };
std::vector<LrcLine> g_lrc;
std::wstring g_line;
int    g_lineStart = 0;
size_t g_shown = static_cast<size_t>(-1);
COLORREF g_textColor = kCream;

// Cursor dance
struct DanceState { bool on; int start; int dur; int shape; POINT from; };
DanceState g_dance = {false, 0, 0, 0, {0, 0}};

// Gamma tint
WORD g_origGamma[3][256];
bool g_hasGamma = false;
float g_tint = 0.f, g_tintTarget = 0.f;       // current / wanted tint strength (0..1)
bool g_heartbeat = false;

// Window minimize / restore choreography
std::vector<HWND> g_toMin, g_minimized, g_toRestore;
int g_nextWinOpMs = 0;

// Hearts overlay
struct Heart { float x, y, size, vy, drift, phase; int tone; };
std::vector<Heart> g_hearts;
float   g_heartRate = 0.f, g_spawnAcc = 0.f;
bool    g_overlayVisible = false;
HDC     g_memDC = nullptr;
HBITMAP g_memBmp = nullptr, g_oldBmp = nullptr;
HBRUSH  g_keyBrush = nullptr, g_heartBrush[4] = {};

// ---------------------------------------------------------------------------
//  Small helpers
// ---------------------------------------------------------------------------
float Clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
float Smooth(float x)  { x = Clamp01(x); return x * x * (3.f - 2.f * x); }
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
float Rand01() { return static_cast<float>(std::rand() % 10000) / 10000.f; }

std::wstring ExeDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p;
    return s.substr(0, s.find_last_of(L"\\/") + 1);
}

std::wstring ReadUtf8File(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";

    DWORD size = GetFileSize(f, nullptr), got = 0;
    std::string data(size, '\0');
    if (size) ReadFile(f, &data[0], size, &got, nullptr);
    CloseHandle(f);
    data.resize(got);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF)
        data.erase(0, 3);
    if (data.empty()) return L"";

    int wlen = MultiByteToWideChar(CP_UTF8, 0, data.data(), static_cast<int>(data.size()), nullptr, 0);
    std::wstring w(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data.data(), static_cast<int>(data.size()), &w[0], wlen);
    return w;
}

// Parses standard LRC: "[mm:ss.xx]text". Metadata tags ([ti:], [ar:], ...) and
// lines with no text are skipped. Several timestamps on one line are supported.
std::vector<LrcLine> LoadLrc(const std::wstring& path) {
    std::vector<LrcLine> out;
    const std::wstring w = ReadUtf8File(path);

    size_t pos = 0;
    while (pos <= w.size()) {
        size_t nl = w.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = w.size();
        std::wstring line = w.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();

        std::vector<int> stamps;
        size_t i = 0;
        while (i + 1 < line.size() && line[i] == L'[' && std::iswdigit(line[i + 1])) {
            size_t close = line.find(L']', i);
            if (close == std::wstring::npos) break;
            std::wstring tag = line.substr(i + 1, close - i - 1);       // e.g. "01:23.45"
            size_t colon = tag.find(L':');
            if (colon == std::wstring::npos) break;
            int mm = _wtoi(tag.substr(0, colon).c_str());
            std::wstring rest = tag.substr(colon + 1);
            size_t dot = rest.find_first_of(L".:");
            int ss = _wtoi(rest.substr(0, dot).c_str());
            int frac = 0;
            if (dot != std::wstring::npos) {
                std::wstring f = rest.substr(dot + 1, 3);
                int v = _wtoi(f.c_str());
                frac = (f.size() == 1) ? v * 100 : (f.size() == 2) ? v * 10 : v;
            }
            stamps.push_back(mm * 60000 + ss * 1000 + frac);
            i = close + 1;
        }
        if (stamps.empty()) continue;                                   // metadata / junk

        std::wstring text = line.substr(i);
        size_t b = text.find_first_not_of(L' ');
        text = (b == std::wstring::npos) ? L"" : text.substr(b);
        if (text.empty()) continue;                                     // blank timing marks

        for (int ms : stamps) out.push_back({ ms, text });
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const LrcLine& a, const LrcLine& b) { return a.ms < b.ms; });
    return out;
}

void ShowLine(const std::wstring& text, int now);   // defined below

// ---------------------------------------------------------------------------
//  Audio (MCI) - the song position drives the whole timeline
// ---------------------------------------------------------------------------
bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring CurDir() {
    wchar_t b[MAX_PATH] = {};
    DWORD n = GetCurrentDirectoryW(MAX_PATH, b);
    std::wstring s(b, n);
    if (!s.empty() && s.back() != L'\\') s += L'\\';
    return s;
}

std::wstring FirstMp3In(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.mp3").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring name = fd.cFileName;
    FindClose(h);
    return dir + name;
}

// song.mp3 next to the exe, then in the working directory, then any .mp3 in either
std::wstring FindSong(const std::wstring& exeDir) {
    const std::wstring dirs[2] = { exeDir, CurDir() };
    for (const std::wstring& d : dirs) if (FileExists(d + L"song.mp3")) return d + L"song.mp3";
    for (const std::wstring& d : dirs) { std::wstring f = FirstMp3In(d); if (!f.empty()) return f; }
    return L"";
}

std::wstring MciError(MCIERROR e) {
    wchar_t buf[256] = {};
    mciGetErrorStringW(e, buf, 256);
    return buf;
}

bool OpenAudio(const std::wstring& path) {
    if (path.empty()) { g_audioInfo = L"No song found - put song.mp3 in the same folder as the exe"; return false; }

    std::wstring p = path;                       // 8.3 path dodges trouble with unusual characters
    wchar_t sp[MAX_PATH] = {};
    DWORD n = GetShortPathNameW(path.c_str(), sp, MAX_PATH);
    if (n > 0 && n < MAX_PATH) p = sp;

    MCIERROR e = mciSendStringW((L"open \"" + p + L"\" type mpegvideo alias bgm").c_str(), nullptr, 0, nullptr);
    if (e) e = mciSendStringW((L"open \"" + p + L"\" alias bgm").c_str(), nullptr, 0, nullptr);  // let MCI choose
    if (e) { g_audioInfo = L"Can't open the song: " + MciError(e); return false; }

    mciSendStringW(L"set bgm time format milliseconds", nullptr, 0, nullptr);
    e = mciSendStringW(L"play bgm", nullptr, 0, nullptr);
    if (e) {
        g_audioInfo = L"Can't play the song: " + MciError(e);
        mciSendStringW(L"close bgm", nullptr, 0, nullptr);
        return false;
    }
    return true;
}

int AudioLengthMs() {
    wchar_t buf[64] = {};
    if (mciSendStringW(L"status bgm length", buf, 64, nullptr) == 0) return _wtoi(buf);
    return 0;
}

bool SongStopped() {                             // true once playback has finished
    if (!g_audioOk) return false;
    wchar_t buf[64] = {};
    return mciSendStringW(L"status bgm mode", buf, 64, nullptr) == 0 && std::wcscmp(buf, L"stopped") == 0;
}

int SongPosMs() {
    static int lastPos = 0;
    int wall = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                   Clock::now() - g_t0).count());
    if (g_audioOk) {
        wchar_t buf[64] = {};
        if (mciSendStringW(L"status bgm position", buf, 64, nullptr) == 0) {
            int p = _wtoi(buf);
            if (p > 0) { lastPos = std::max(lastPos, p); return lastPos; }
        }
        if (wall < 3000) return 0;   // give audio a moment to start...
        g_audioOk = false;           // ...then fall back to the wall clock
        ShowLine(L"Song isn't reporting playback - running on a timer", wall);
    }
    return wall;
}

// ---------------------------------------------------------------------------
//  Gamma tint (safe: backed up at start, restored on every exit path)
// ---------------------------------------------------------------------------
void BackupGamma() {
    HDC dc = GetDC(nullptr);
    if (!dc) return;
    g_hasGamma = GetDeviceGammaRamp(dc, g_origGamma) != FALSE;
    ReleaseDC(nullptr, dc);
}

void RestoreGamma() {
    if (!g_hasGamma) return;
    HDC dc = GetDC(nullptr);
    if (!dc) return;
    SetDeviceGammaRamp(dc, g_origGamma);
    ReleaseDC(nullptr, dc);
}

void ApplyTint(float s, float pulse) {
    if (!g_hasGamma) return;                     // never build a ramp from garbage
    HDC dc = GetDC(nullptr);
    if (!dc) return;
    const float f[3] = {
        1.f + 0.22f * s + 0.18f * pulse * s,     // red: warm + heartbeat glow
        1.f - 0.05f * s - 0.08f * pulse * s,     // green
        1.f - 0.30f * s                          // blue: sepia
    };
    WORD ramp[3][256];
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++)
            ramp[c][i] = static_cast<WORD>(std::min(65535.f, g_origGamma[c][i] * f[c]));
    SetDeviceGammaRamp(dc, ramp);                // may be refused by some drivers; harmless
    ReleaseDC(nullptr, dc);
}

// Double-thump at 60 bpm (slow and soft on purpose - no strobing)
float Heartbeat(int ms) {
    float t = std::fmod(ms / 1000.f, 1.f);
    auto bump = [&](float c) { float d = t - c; return std::exp(-d * d * 600.f); };
    return bump(0.05f) + 0.7f * bump(0.28f);
}

void UpdateTint(int ms, float dt) {
    g_tint += (g_tintTarget - g_tint) * (1.f - std::exp(-dt * 1.2f));   // ~3 s to settle
    static float acc = 0.f;
    static bool applied = false;
    acc += dt;
    if (acc < 0.04f) return;                     // ~25 updates per second
    acc = 0.f;
    if (g_tint <= 0.003f) {
        if (applied) { RestoreGamma(); applied = false; }
        return;
    }
    ApplyTint(g_tint, g_heartbeat ? Heartbeat(ms) : 0.f);
    applied = true;
}

// ---------------------------------------------------------------------------
//  Effects
// ---------------------------------------------------------------------------
void SetTone(COLORREF c) {
    g_textColor = c;
    if (g_hStatic) InvalidateRect(g_hStatic, nullptr, TRUE);
}

void ShowLine(const std::wstring& text, int now) {
    g_line = text;
    g_lineStart = now;
    g_shown = static_cast<size_t>(-1);
}

void UpdateLyricText(int ms) {                   // typewriter reveal
    size_t n = std::min(g_line.size(), static_cast<size_t>(std::max(0, (ms - g_lineStart) / 38)));
    if (n != g_shown) {
        g_shown = n;
        SetWindowTextW(g_hStatic, g_line.substr(0, n).c_str());
    }
}

void StartShake(int now, float amp, int dur) { g_shake.start = now; g_shake.dur = dur; g_shake.amp = amp; }

void StartDance(int now, int dur, int shape) {   // shape: 0 = heart, 1 = infinity
    g_dance.on = true; g_dance.start = now; g_dance.dur = dur; g_dance.shape = shape;
    GetCursorPos(&g_dance.from);
}

// Cursor traces a heart or an infinity sign (two loops), gliding in from wherever it was
void UpdateDance(int ms) {
    if (!g_dance.on) return;
    float u = (ms - g_dance.start) / static_cast<float>(g_dance.dur);
    if (u >= 1.f) { g_dance.on = false; return; }
    float t = u * 4.f * kPi;
    float hx, hy;
    if (g_dance.shape == 0) {                    // heart
        float s = std::sin(t);
        hx = 16.f * s * s * s;
        hy = -(13.f * std::cos(t) - 5.f * std::cos(2 * t) - 2.f * std::cos(3 * t) - std::cos(4 * t)) - 5.f;
    } else {                                     // infinity (lemniscate-style)
        hx = 24.f * std::sin(t);
        hy = 12.f * std::sin(2.f * t);
    }
    float sc = g_sh / 50.f;
    float tx = g_sw / 2.f + hx * sc;
    float ty = g_sh / 2.f + hy * sc;
    float e = Smooth(u / 0.1f);
    SetCursorPos(static_cast<int>(Lerp(static_cast<float>(g_dance.from.x), tx, e)),
                 static_cast<int>(Lerp(static_cast<float>(g_dance.from.y), ty, e)));
}

void UpdateWindowMotion(int ms, float dt) {
    if (g_follow) {                              // eased "stalker" follow
        POINT pt; GetCursorPos(&pt);
        float k = 1.f - std::exp(-dt * 7.f);
        g_winX = Lerp(g_winX, pt.x - g_winW / 2.f, k);
        g_winY = Lerp(g_winY, pt.y - g_winH / 2.f, k);
    }
    if (g_shake.dur > 0 && ms < g_shake.start + g_shake.dur) {
        if (ms - g_lastShakeMs >= 16) {
            float a = g_shake.amp * (1.f - (ms - g_shake.start) / static_cast<float>(g_shake.dur));
            g_shakeX = (Rand01() * 2.f - 1.f) * a;
            g_shakeY = (Rand01() * 2.f - 1.f) * a;
            g_lastShakeMs = ms;
        }
    } else {
        g_shakeX = g_shakeY = 0.f;
    }
    int x = static_cast<int>(std::lround(g_winX + g_shakeX));
    int y = static_cast<int>(std::lround(g_winY + g_shakeY));
    if (x != g_lastX || y != g_lastY) {
        SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        g_lastX = x; g_lastY = y;
    }
}

// The OK button runs away whenever the cursor gets close.
// (Polled here because mouse-move over a child control never reaches the parent.)
void UpdateDodge() {
    if (!g_dodge || !g_hBtn) return;
    POINT pt; GetCursorPos(&pt);
    RECT br; GetWindowRect(g_hBtn, &br);
    InflateRect(&br, 36, 36);
    if (!PtInRect(&br, pt)) return;

    POINT cp = pt; ScreenToClient(g_hwnd, &cp);
    RECT cr; GetClientRect(g_hwnd, &cr);
    const int bw = 92, bh = 30;
    int bestX = 10, bestY = 130;
    float bestD = -1.f;
    for (int i = 0; i < 14; i++) {               // pick the candidate farthest from the cursor
        int x = 10 + std::rand() % std::max(1, static_cast<int>(cr.right) - bw - 20);
        int y = 124 + std::rand() % std::max(1, static_cast<int>(cr.bottom) - bh - 124 - 10);
        float dx = static_cast<float>(x + bw / 2 - cp.x), dy = static_cast<float>(y + bh / 2 - cp.y);
        float d = dx * dx + dy * dy;
        if (d > bestD) { bestD = d; bestX = x; bestY = y; }
    }
    SetWindowPos(g_hBtn, nullptr, bestX, bestY, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
}

// --- Windows slide away one by one, then come back at the finale -----------
BOOL CALLBACK CollectProc(HWND h, LPARAM) {
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    if (GetWindow(h, GW_OWNER)) return TRUE;
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (!(GetWindowLongPtrW(h, GWL_STYLE) & WS_MINIMIZEBOX)) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;          // never touch our own windows
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    wchar_t title[2] = {};
    if (!GetWindowTextW(h, title, 2)) return TRUE;
    g_toMin.push_back(h);
    return TRUE;
}

void BeginMinimizeAll(int now) {
    g_toMin.clear();
    EnumWindows(CollectProc, 0);
    g_nextWinOpMs = now;
}

void BeginRestoreAll(int now) {
    g_toMin.clear();
    g_toRestore.assign(g_minimized.rbegin(), g_minimized.rend());
    g_minimized.clear();
    g_nextWinOpMs = now;
}

void StepWindowOps(int now) {
    if (now < g_nextWinOpMs) return;
    if (!g_toMin.empty()) {
        HWND h = g_toMin.back(); g_toMin.pop_back();
        if (IsWindow(h)) { ShowWindowAsync(h, SW_SHOWMINNOACTIVE); g_minimized.push_back(h); }
        g_nextWinOpMs = now + 140;
    } else if (!g_toRestore.empty()) {
        HWND h = g_toRestore.back(); g_toRestore.pop_back();
        if (IsWindow(h) && IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
        g_nextWinOpMs = now + 140;
    }
}

void FlushWindowOps() {                          // used on exit: put everything back now
    for (HWND h : g_minimized) if (IsWindow(h) && IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    for (HWND h : g_toRestore) if (IsWindow(h) && IsIconic(h)) ShowWindowAsync(h, SW_RESTORE);
    g_toMin.clear(); g_minimized.clear(); g_toRestore.clear();
}

// --- Floating hearts overlay (click-through, colour-keyed) -----------------
void InitOverlay(HINSTANCE hi) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = hi;
    wc.lpszClassName = L"LoveShowOverlay";
    RegisterClassW(&wc);

    g_overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, g_sw, g_sh, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(g_overlay, kKey, 0, LWA_COLORKEY);

    HDC scr = GetDC(nullptr);
    g_memDC  = CreateCompatibleDC(scr);
    g_memBmp = CreateCompatibleBitmap(scr, g_sw, g_sh);
    ReleaseDC(nullptr, scr);
    g_oldBmp = static_cast<HBITMAP>(SelectObject(g_memDC, g_memBmp));

    g_keyBrush = CreateSolidBrush(kKey);
    const COLORREF pal[4] = { RGB(255, 70, 110), RGB(255, 120, 150), RGB(230, 40, 80), RGB(255, 175, 195) };
    for (int i = 0; i < 4; i++) g_heartBrush[i] = CreateSolidBrush(pal[i]);
}

void SpawnHeart(bool burst) {
    if (g_hearts.size() > 160) return;
    float sc = g_sh / 1080.f;
    Heart h;
    h.size  = (18.f + std::rand() % 46) * sc;
    h.x     = static_cast<float>(std::rand() % std::max(1, g_sw));
    h.y     = g_sh + h.size + (burst ? Rand01() * g_sh * 0.25f : 0.f);
    h.vy    = (70.f + std::rand() % 170) * sc * (burst ? 1.7f : 1.f);
    h.drift = (15.f + std::rand() % 45) * sc;
    h.phase = Rand01() * 6.28f;
    h.tone  = std::rand() % 4;
    g_hearts.push_back(h);
}

void Burst(int n) { for (int i = 0; i < n; i++) SpawnHeart(true); }

void UpdateOverlay(int ms, float dt) {
    static float acc = 0.f;
    g_spawnAcc += g_heartRate * dt;
    while (g_spawnAcc >= 1.f) { g_spawnAcc -= 1.f; SpawnHeart(false); }
    for (Heart& h : g_hearts) {
        h.y -= h.vy * dt;
        h.x += std::sin(ms / 1000.f * 1.7f + h.phase) * h.drift * dt;
    }
    g_hearts.erase(std::remove_if(g_hearts.begin(), g_hearts.end(),
                   [](const Heart& h) { return h.y < -h.size; }), g_hearts.end());

    bool want = g_heartRate > 0.f || !g_hearts.empty();
    if (want && !g_overlayVisible) {
        ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        g_overlayVisible = true;
    } else if (!want && g_overlayVisible) {
        ShowWindow(g_overlay, SW_HIDE);
        g_overlayVisible = false;
    }
    if (!g_overlayVisible) return;

    acc += dt;
    if (acc < 1.f / 30.f) return;                // draw at ~30 fps
    acc = 0.f;

    RECT r = { 0, 0, g_sw, g_sh };
    FillRect(g_memDC, &r, g_keyBrush);
    HGDIOBJ oldPen = SelectObject(g_memDC, GetStockObject(NULL_PEN));
    for (const Heart& h : g_hearts) {
        POINT p[36];
        for (int i = 0; i < 36; i++) {
            float t = i * 2.f * kPi / 36.f, s = std::sin(t);
            float x = 16.f * s * s * s;
            float y = -(13.f * std::cos(t) - 5.f * std::cos(2 * t) - 2.f * std::cos(3 * t) - std::cos(4 * t));
            p[i].x = static_cast<LONG>(h.x + x * h.size / 34.f);
            p[i].y = static_cast<LONG>(h.y + y * h.size / 34.f);
        }
        HGDIOBJ oldBrush = SelectObject(g_memDC, g_heartBrush[h.tone]);
        Polygon(g_memDC, p, 36);
        SelectObject(g_memDC, oldBrush);
    }
    SelectObject(g_memDC, oldPen);

    HDC dc = GetDC(g_overlay);
    BitBlt(dc, 0, 0, g_sw, g_sh, g_memDC, 0, 0, SRCCOPY);
    ReleaseDC(g_overlay, dc);
}

// ---------------------------------------------------------------------------
//  Effect timeline (song position in ms). Lyric text is NOT stored here - it is
//  shown straight from the .lrc file. Times follow this song's structure.
// ---------------------------------------------------------------------------
struct Fx { int ms; void (*fn)(int now); };

const Fx kFx[] = {
    // --- verse 1 ---
    {  17140, [](int)   { g_follow = true; } },                               // 0:17  popup stalks the cursor
    {  24840, [](int)   { g_follow = false; g_dodge = true; SetTone(kPink); } }, // 0:24  OK button starts running
    {  28210, [](int)   { g_tintTarget = 0.8f; } },                           // 0:28  sepia fades in
    {  32710, [](int)   { g_heartRate = 1.5f; } },                            // 0:32  hearts begin
    {  37780, [](int n) { StartDance(n, 5000, 0); g_heartRate = 3.f; } },     // 0:37  cursor draws a heart
    {  41290, [](int n) { BeginMinimizeAll(n); } },                           // 0:41  windows slide away
    // --- chorus 1 ---
    {  47890, [](int n) { StartShake(n, 8.f, 500); g_heartRate = 5.f; } },
    {  54820, [](int n) { StartShake(n, 15.f, 800); g_heartbeat = true; g_tintTarget = 1.f;
                          g_heartRate = 7.f; SetTone(kRed); Burst(40); } },
    // --- verse 2: calm, windows come back ---
    {  64930, [](int n) { BeginRestoreAll(n); g_heartbeat = false; g_tintTarget = 0.6f;
                          g_heartRate = 1.5f; SetTone(kPink); } },
    {  79690, [](int n) { StartShake(n, 22.f, 1200); g_heartRate = 8.f; Burst(60); } },  // 1:19 "vibrations"
    {  83150, [](int)   { g_tintTarget = 0.9f; } },
    {  92610, [](int n) { StartDance(n, 5000, 1); g_heartRate = 4.f; } },     // 1:32  cursor draws an infinity sign
    {  96320, [](int n) { BeginMinimizeAll(n); } },                           // 1:36  windows leave again
    // --- chorus 2 ---
    { 103020, [](int n) { StartShake(n, 8.f, 500); g_heartRate = 6.f; } },
    { 109750, [](int n) { StartShake(n, 15.f, 800); g_heartbeat = true; g_tintTarget = 1.f;
                          SetTone(kRed); Burst(40); } },
    { 116480, [](int n) { StartShake(n, 20.f, 1000); Burst(70); g_heartRate = 9.f; } },
    // --- instrumental: windows return, popup stalks again ---
    { 123100, [](int n) { BeginRestoreAll(n); g_heartbeat = false; g_follow = true;
                          g_tintTarget = 0.7f; g_heartRate = 2.f; SetTone(kPink); } },
    // --- bridge ---
    { 135160, [](int)   { g_follow = false; g_tintTarget = 0.5f; g_heartRate = 1.f; } },
    { 141880, [](int)   { g_heartRate = 3.f; } },
    { 147490, [](int n) { StartDance(n, 5000, 0); g_heartRate = 4.f; } },     // 2:27  third dance
    { 151190, [](int)   { g_tintTarget = 0.8f; g_heartRate = 6.f; } },
    // --- final chorus: builds to the cut-off lines, then the big one ---
    { 157560, [](int n) { StartShake(n, 10.f, 600); g_heartbeat = true; SetTone(kRed); g_heartRate = 8.f; } },
    { 164430, [](int n) { StartShake(n, 15.f, 700); Burst(40); } },
    { 168010, [](int n) { StartShake(n, 20.f, 800); Burst(40); } },
    { 171340, [](int n) { StartShake(n, 26.f, 1600); Burst(100); g_heartRate = 14.f; g_tintTarget = 1.f; } },
    // --- finale (the final short line): everything calms down, button becomes catchable ---
    { 185620, [](int n) {
                          g_dodge = false; g_heartbeat = false; g_heartRate = 0.f; g_tintTarget = 0.f;
                          Burst(80); StartShake(n, 6.f, 400); BeginRestoreAll(n); SetTone(kCream);
                          SetWindowTextW(g_hBtn, L"OK \u2665"); } },
};
constexpr size_t kFxCount = sizeof(kFx) / sizeof(kFx[0]);

// ---------------------------------------------------------------------------
//  Window procedure
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, g_textColor);
        SetBkColor(dc, kBg);
        return reinterpret_cast<LRESULT>(g_bgBrush);
    }
    case WM_COMMAND:
        if (LOWORD(wp) == kOkId) DestroyWindow(hwnd);   // only catchable when not dodging
        return 0;
    case WM_DESTROY:
        RestoreGamma();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LONG WINAPI CrashHandler(EXCEPTION_POINTERS*) {      // never leave the screen tinted
    RestoreGamma();
    return EXCEPTION_CONTINUE_SEARCH;
}

void Cleanup() {
    FlushWindowOps();
    RestoreGamma();
    mciSendStringW(L"stop bgm", nullptr, 0, nullptr);
    mciSendStringW(L"close bgm", nullptr, 0, nullptr);
    if (g_overlay) DestroyWindow(g_overlay);
    if (g_hwnd && IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
    if (g_memDC) { SelectObject(g_memDC, g_oldBmp); DeleteDC(g_memDC); }
    if (g_memBmp) DeleteObject(g_memBmp);
    if (g_keyBrush) DeleteObject(g_keyBrush);
    for (HBRUSH b : g_heartBrush) if (b) DeleteObject(b);
    if (g_fontText) DeleteObject(g_fontText);
    if (g_fontBtn) DeleteObject(g_fontBtn);
    if (g_bgBrush) DeleteObject(g_bgBrush);
    timeEndPeriod(1);
}

} // namespace

// ---------------------------------------------------------------------------
//  Entry point
// ---------------------------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    timeBeginPeriod(1);
    SetUnhandledExceptionFilter(CrashHandler);
    BackupGamma();
    std::atexit(RestoreGamma);

    const std::wstring dir = ExeDir();
    g_lrc     = LoadLrc(dir + L"lyrics.lrc");
    g_audioOk = OpenAudio(FindSong(dir));
    g_t0 = Clock::now();

    {   // show length: the real audio length if we can get it
        int len = g_audioOk ? AudioLengthMs() : 0;
        int lastLyric = g_lrc.empty() ? 0 : g_lrc.back().ms;
        g_endMs = len > 0 ? len : std::max(kFallbackEndMs, lastLyric + 8000);
    }

    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);

    // --- popup window ---
    g_bgBrush = CreateSolidBrush(kBg);
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = g_bgBrush;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"LoveShowWindow";
    RegisterClassW(&wc);

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exSt  = WS_EX_TOPMOST | WS_EX_LAYERED;
    RECT wr = { 0, 0, 460, 210 };                       // client size
    AdjustWindowRectEx(&wr, style, FALSE, exSt);
    g_winW = wr.right - wr.left;
    g_winH = wr.bottom - wr.top;
    g_winX = (g_sw - g_winW) / 2.f;
    g_winY = (g_sh - g_winH) / 2.f;

    g_hwnd = CreateWindowExW(exSt, wc.lpszClassName, L"I Think They Call This Love", style,
                             static_cast<int>(g_winX), static_cast<int>(g_winY), g_winW, g_winH,
                             nullptr, nullptr, hInst, nullptr);
    SetLayeredWindowAttributes(g_hwnd, 0, 0, LWA_ALPHA);   // start invisible, fade in

    g_hStatic = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                                20, 16, 420, 100, g_hwnd, nullptr, hInst, nullptr);
    g_hBtn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             184, 140, 92, 30, g_hwnd, reinterpret_cast<HMENU>(kOkId), hInst, nullptr);

    g_fontText = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_fontBtn  = CreateFontW(17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(g_hStatic, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontText), TRUE);
    SendMessageW(g_hBtn, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBtn), TRUE);

    InitOverlay(hInst);
    {
        std::wstring hint = L"(press ESC at any time to stop)";
        if (!g_audioInfo.empty()) hint = g_audioInfo;
        if (g_lrc.empty()) hint += L"\r\nlyrics.lrc not found next to the exe";
        ShowLine(hint, 0);
    }
    ShowWindow(g_hwnd, SW_SHOW);

    // --- main loop ---
    size_t lyr = 0, fx = 0;
    int lastAlpha = -1;
    int nextStopCheck = 5000;
    auto lastWall = Clock::now();
    bool quit = false;
    MSG msg;

    while (!quit) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;       // emergency exit

        auto wallNow = Clock::now();
        float dt = std::min(0.1f, std::chrono::duration<float>(wallNow - lastWall).count());
        lastWall = wallNow;
        int ms = SongPosMs();

        // lyrics + effects (while-loops so a hiccup never skips a line or an effect)
        while (lyr < g_lrc.size() && ms >= g_lrc[lyr].ms) { ShowLine(g_lrc[lyr].text, ms); lyr++; }
        while (fx < kFxCount && ms >= kFx[fx].ms)        { kFx[fx].fn(ms); fx++; }

        // popup fade in / out
        float a = std::min(Smooth(ms / 1500.f), 1.f - Smooth((ms - (g_endMs - 1800)) / 1800.f));
        int alpha = static_cast<int>(a * 255.f);
        if (alpha != lastAlpha) {
            SetLayeredWindowAttributes(g_hwnd, 0, static_cast<BYTE>(alpha), LWA_ALPHA);
            lastAlpha = alpha;
        }

        UpdateLyricText(ms);
        UpdateDance(ms);
        UpdateWindowMotion(ms, dt);
        UpdateDodge();
        StepWindowOps(ms);
        UpdateTint(ms, dt);
        UpdateOverlay(ms, dt);

        if (ms >= g_endMs) break;
        if (ms >= nextStopCheck) {                       // playback finished a bit early?
            nextStopCheck = ms + 500;
            if (SongStopped()) break;
        }
        Sleep(8);
    }

    Cleanup();
    return 0;
}
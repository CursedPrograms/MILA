// MILA WiFi controller (native Win32) — C++ port of mila_controller.py.
// Mirrors the onboard web dashboard. No third-party dependencies: GDI for UI,
// WinHTTP for transport.
//
// USAGE
//   mila_controller.exe
//   mila_controller.exe --host 192.168.4.1 --port 5010
//
//   If MILA joined NORA's network (fleet mode) instead of hosting her own
//   AP, she won't be at 192.168.4.1 anymore — check NORA's dashboard/fleet
//   registry (http://192.168.4.1:5000/robots) for MILA's actual IP and pass
//   it with --host.
//
// BUILD
//   cmake -B build -G "MinGW Makefiles" && cmake --build build
//   or: g++ -std=c++17 -O2 -mwindows -static mila_controller.cpp -o mila_controller.exe
//           -lwinhttp -lgdi32 -luser32 -lshell32
//
// CONTROLS
//   1 / 2 / 3     WASD / TANK / OBSTACLE mode
//   WASD mode:    Arrow keys or WASD to drive, Space to stop
//   TANK mode:    Q/A = left track fwd/back, E/D = right track fwd/back
//   Esc           quit
//   Everything is also clickable with the mouse.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ----------------------------------------------------------------------------
// Transport
// ----------------------------------------------------------------------------

// Minimal parser for the firmware's flat /status JSON: {"k":"str","k2":12,...}
static std::map<std::string, std::string> parseFlatJson(const std::string& s) {
    std::map<std::string, std::string> out;
    size_t i = 0, n = s.size();
    auto readString = [&](std::string& dst) {
        dst.clear();
        ++i;  // opening quote
        while (i < n && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < n) ++i;
            dst += s[i++];
        }
        ++i;  // closing quote
    };
    while (i < n) {
        while (i < n && s[i] != '"') ++i;
        if (i >= n) break;
        std::string key, val;
        readString(key);
        while (i < n && s[i] != ':') ++i;
        ++i;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i < n && s[i] == '"') {
            readString(val);
        } else {
            while (i < n && s[i] != ',' && s[i] != '}') val += s[i++];
            while (!val.empty() && (val.back() == ' ' || val.back() == '\r' || val.back() == '\n')) val.pop_back();
        }
        out[key] = val;
    }
    return out;
}

class WifiLink {
public:
    WifiLink(const std::wstring& host, int port) : host_(host), port_(port) {
        session_ = WinHttpOpen(L"MILA-Controller/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_) WinHttpSetTimeouts(session_, 1000, 1000, 1000, 1000);
        poller_ = std::thread([this] { poll(); });
    }

    ~WifiLink() {
        running_ = false;
        if (poller_.joinable()) poller_.join();
        if (session_) WinHttpCloseHandle(session_);
    }

    bool ok() const { return ok_; }

    std::map<std::string, std::string> telemetry() {
        std::lock_guard<std::mutex> lk(lock_);
        return data_;
    }

    void cmd(const std::string& action) { fireAndForget("/cmd?v=" + action); }
    void mode(const std::string& m) { fireAndForget("/mode?v=" + m); }

private:
    bool get(const std::string& path, std::string* body) {
        if (!session_) return false;
        bool success = false;
        std::wstring wpath(path.begin(), path.end());
        HINTERNET conn = WinHttpConnect(session_, host_.c_str(), (INTERNET_PORT)port_, 0);
        if (conn) {
            HINTERNET req = WinHttpOpenRequest(conn, L"GET", wpath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
            if (req) {
                if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(req, nullptr)) {
                    success = true;
                    DWORD avail = 0;
                    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                        std::string chunk(avail, '\0');
                        DWORD read = 0;
                        if (!WinHttpReadData(req, &chunk[0], avail, &read)) break;
                        if (body) body->append(chunk, 0, read);
                    }
                }
                WinHttpCloseHandle(req);
            }
            WinHttpCloseHandle(conn);
        }
        return success;
    }

    void fireAndForget(const std::string& path) {
        std::thread([this, path] { get(path, nullptr); }).detach();
    }

    void poll() {
        while (running_) {
            std::string body;
            if (get("/status", &body)) {
                auto parsed = parseFlatJson(body);
                std::lock_guard<std::mutex> lk(lock_);
                data_ = std::move(parsed);
                ok_ = true;
            } else {
                ok_ = false;
            }
            for (int i = 0; i < 8 && running_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    std::wstring host_;
    int port_;
    HINTERNET session_ = nullptr;
    std::atomic<bool> running_{true};
    std::atomic<bool> ok_{false};
    std::mutex lock_;
    std::map<std::string, std::string> data_;
    std::thread poller_;
};

// ----------------------------------------------------------------------------
// UI — same dark/cyan palette as the onboard dashboard
// ----------------------------------------------------------------------------

static const COLORREF BG        = RGB(10, 10, 15);
static const COLORREF PANEL     = RGB(18, 18, 26);
static const COLORREF BORDER    = RGB(30, 30, 46);
static const COLORREF ACCENT    = RGB(0, 212, 255);
static const COLORREF RED       = RGB(255, 68, 68);
static const COLORREF GREEN     = RGB(0, 255, 136);
static const COLORREF TEXT      = RGB(224, 224, 240);
static const COLORREF DIM       = RGB(102, 102, 136);
static const COLORREF ACCENT_BG = RGB(14, 34, 46);

static const int W = 420, H = 720;

struct Button {
    RECT rect;
    std::wstring label;
    std::function<void()> cb;
    bool active = false;
    bool pressed = false;
    int group = 0;  // 0 always, 1 WASD-only, 2 TANK-only

    bool hit(POINT p) const { return PtInRect(&rect, p) != 0; }
};

enum Group { ALWAYS = 0, WASD_ONLY = 1, TANK_ONLY = 2 };

struct App {
    std::wstring hostW;
    std::string hostA;
    int port = 5010;
    WifiLink* link = nullptr;

    std::string mode = "wasd";  // mirrors the firmware's WASD boot default
    std::string activeDrive;
    int speedPct = 100;
    std::vector<Button> buttons;
    size_t modeBtn[3] = {0, 0, 0};
    size_t stopBtn = 0;

    HFONT fBig = nullptr, fMed = nullptr, fSml = nullptr;
    HBITMAP backBmp = nullptr;
    HDC backDc = nullptr;
};

static App g;

static void startDrive(const std::string& action) {
    g.activeDrive = action;
    g.link->cmd(action);
}

static void stopDrive() {
    if (!g.activeDrive.empty()) {
        g.activeDrive.clear();
        g.link->cmd("STOP");
    }
}

static void setMode(const std::string& m) {
    g.mode = m;
    g.link->mode(m);
    const char* names[3] = {"wasd", "tank", "obstacle"};
    for (int i = 0; i < 3; ++i) g.buttons[g.modeBtn[i]].active = (m == names[i]);
}

static void adjustSpeed(int delta) {
    g.speedPct = std::max(25, std::min(100, g.speedPct + delta));
    g.link->cmd("SPEED:" + std::to_string(g.speedPct));
}

static size_t addButton(int x, int y, int w, int h, const wchar_t* label, std::function<void()> cb,
                        int group = ALWAYS, bool active = false) {
    Button b;
    b.rect = {x, y, x + w, y + h};
    b.label = label;
    b.cb = std::move(cb);
    b.group = group;
    b.active = active;
    g.buttons.push_back(std::move(b));
    return g.buttons.size() - 1;
}

static void buildButtons() {
    g.modeBtn[0] = addButton(20, 100, 120, 40, L"WASD", [] { setMode("wasd"); }, ALWAYS, true);
    g.modeBtn[1] = addButton(150, 100, 120, 40, L"TANK", [] { setMode("tank"); });
    g.modeBtn[2] = addButton(280, 100, 120, 40, L"OBSTACLE", [] { setMode("obstacle"); });

    // Speed presets mirror the firmware's speedPresets[] (100/75/50/25).
    addButton(20, 150, 180, 40, L"SPD -", [] { adjustSpeed(-25); });
    addButton(220, 150, 180, 40, L"SPD +", [] { adjustSpeed(25); });

    // WASD d-pad
    const int cx = W / 2, cy = 280, bs = 70, gap = 8;
    addButton(cx - bs / 2, cy - bs - gap, bs, bs, L"^", [] { startDrive("FORWARD"); }, WASD_ONLY);
    addButton(cx - bs / 2, cy + gap, bs, bs, L"v", [] { startDrive("BACKWARD"); }, WASD_ONLY);
    addButton(cx - bs - gap - bs / 2 - gap, cy - bs / 2, bs, bs, L"<", [] { startDrive("LEFT"); }, WASD_ONLY);
    addButton(cx + gap + bs / 2 + gap, cy - bs / 2, bs, bs, L">", [] { startDrive("RIGHT"); }, WASD_ONLY);
    g.stopBtn = addButton(cx - bs / 2, cy - bs / 2, bs, bs, L"STOP", [] { g.link->cmd("STOP"); }, WASD_ONLY);

    // TANK track buttons
    const int ty = 280;
    addButton(60, ty - 40, 90, 50, L"Q\nL-FWD", [] { startDrive("L_FWD"); }, TANK_ONLY);
    addButton(60, ty + 40, 90, 50, L"A\nL-BWD", [] { startDrive("L_BWD"); }, TANK_ONLY);
    addButton(W - 150, ty - 40, 90, 50, L"E\nR-FWD", [] { startDrive("R_FWD"); }, TANK_ONLY);
    addButton(W - 150, ty + 40, 90, 50, L"D\nR-BWD", [] { startDrive("R_BWD"); }, TANK_ONLY);
}

static bool visible(const Button& b) {
    return b.group == ALWAYS || (b.group == WASD_ONLY && g.mode == "wasd") ||
           (b.group == TANK_ONLY && g.mode == "tank");
}

// Key -> action tables
static const char* keyDrive(WPARAM vk) {
    if (g.mode == "wasd") {
        switch (vk) {
            case VK_UP: case 'W': return "FORWARD";
            case VK_DOWN: case 'S': return "BACKWARD";
            case VK_LEFT: case 'A': return "LEFT";
            case VK_RIGHT: case 'D': return "RIGHT";
        }
    } else if (g.mode == "tank") {
        switch (vk) {
            case 'Q': return "L_FWD";
            case 'A': return "L_BWD";
            case 'E': return "R_FWD";
            case 'D': return "R_BWD";
        }
    }
    return nullptr;
}

// ---- drawing ----

static void fillRound(HDC dc, const RECT& r, COLORREF fill, COLORREF line, int lineW, int radius) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, lineW, line);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

static void drawText(HDC dc, HFONT font, COLORREF col, const std::wstring& s, int x, int y, UINT fmt = DT_LEFT) {
    SelectObject(dc, font);
    SetTextColor(dc, col);
    RECT r = {x, y, x + 1000, y + 100};
    if (fmt & DT_CENTER) r = {x - 500, y, x + 500, y + 100};
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

static void drawButton(HDC dc, const Button& b) {
    bool on = b.active || b.pressed;
    fillRound(dc, b.rect, on ? ACCENT_BG : PANEL, on ? ACCENT : BORDER, 2, 8);
    SelectObject(dc, g.fMed);
    SetTextColor(dc, on ? ACCENT : TEXT);
    RECT r = b.rect;
    size_t nl = b.label.find(L'\n');
    if (nl == std::wstring::npos) {
        DrawTextW(dc, b.label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        std::wstring a = b.label.substr(0, nl), c = b.label.substr(nl + 1);
        RECT r1 = r, r2 = r;
        r1.bottom = (r.top + r.bottom) / 2;
        r2.top = r1.bottom;
        DrawTextW(dc, a.c_str(), -1, &r1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        DrawTextW(dc, c.c_str(), -1, &r2, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
}

static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

static void statTile(HDC dc, int x, int y, int w, const std::wstring& label, const std::wstring& value) {
    RECT box = {x, y, x + w, y + 46};
    fillRound(dc, box, PANEL, BORDER, 1, 6);
    drawText(dc, g.fSml, DIM, label, x + 8, y + 6);
    drawText(dc, g.fMed, ACCENT, value, x + 8, y + 22);
}

static std::string tget(const std::map<std::string, std::string>& t, const char* k, const char* def = "--") {
    auto it = t.find(k);
    return it == t.end() ? def : it->second;
}

static void render(HDC dc) {
    RECT full = {0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(BG);
    FillRect(dc, &full, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    auto t = g.link->telemetry();
    auto sp = t.find("speed");
    if (sp != t.end()) g.speedPct = (int)std::strtod(sp->second.c_str(), nullptr);

    drawText(dc, g.fBig, ACCENT, L"MILA", W / 2, 14, DT_CENTER);
    bool ok = g.link->ok();
    std::wstring sub = widen(g.hostA) + L":" + std::to_wstring(g.port) + L"   " + (ok ? L"connected" : L"OFFLINE");
    drawText(dc, g.fSml, ok ? GREEN : RED, sub, W / 2, 48, DT_CENTER);

    for (const auto& b : g.buttons)
        if (visible(b)) drawButton(dc, b);

    const int sy = 400, colw = (W - 50) / 2, c2 = 30 + colw;
    statTile(dc, 20, sy, colw, L"FRONT CM", widen(tget(t, "dist")));
    statTile(dc, c2, sy, colw, L"LEFT CM", widen(tget(t, "left")));
    statTile(dc, 20, sy + 54, colw, L"RIGHT CM", widen(tget(t, "right")));
    statTile(dc, c2, sy + 54, colw, L"LAST CMD", widen(tget(t, "cmd")));
    statTile(dc, 20, sy + 108, colw, L"LAST IR", widen(tget(t, "ir")));
    statTile(dc, c2, sy + 108, colw, L"TEMP C", widen(tget(t, "temp")));
    statTile(dc, 20, sy + 162, colw, L"HUMIDITY %", widen(tget(t, "hum")));
    statTile(dc, c2, sy + 162, colw, L"IP  " + widen(tget(t, "ip")),
             tget(t, "fleet", "") == "1" ? L"FLEET (joined NORA)" : L"STANDALONE AP");
    statTile(dc, 20, sy + 216, colw, L"SPEED %", widen(tget(t, "speed")));
    statTile(dc, c2, sy + 216, colw, L"GUARD", tget(t, "guard", "") == "1" ? L"ACTIVE" : L"clear");

    drawText(dc, g.fSml, DIM, L"1/2/3 mode  -  arrows/WASD or QAED drive  -  space stop  -  esc quit", W / 2, H - 24,
             DT_CENTER);
}

// ---- window ----

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HDC dc = GetDC(hwnd);
            g.backDc = CreateCompatibleDC(dc);
            g.backBmp = CreateCompatibleBitmap(dc, W, H);
            SelectObject(g.backDc, g.backBmp);
            ReleaseDC(hwnd, dc);
            g.fBig = CreateFontW(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 FIXED_PITCH, L"Consolas");
            g.fMed = CreateFontW(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 FIXED_PITCH, L"Consolas");
            g.fSml = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                                 FIXED_PITCH, L"Consolas");
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;
        }
        case WM_TIMER:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            render(g.backDc);
            BitBlt(dc, 0, 0, W, H, g.backDc, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_KEYDOWN: {
            if (lp & (1 << 30)) return 0;  // ignore OS auto-repeat
            if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
            if (wp == '1') setMode("wasd");
            else if (wp == '2') setMode("tank");
            else if (wp == '3') setMode("obstacle");
            else if (wp == VK_SPACE) stopDrive();
            else if (const char* a = keyDrive(wp)) startDrive(a);
            return 0;
        }
        case WM_KEYUP: {
            const char* a = keyDrive(wp);
            if (a && g.activeDrive == a) stopDrive();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
            for (auto& b : g.buttons) {
                if (visible(b) && b.hit(p)) {
                    b.pressed = true;
                    b.cb();
                    break;
                }
            }
            return 0;
        }
        case WM_LBUTTONUP:
            ReleaseCapture();
            for (auto& b : g.buttons) b.pressed = false;
            stopDrive();
            return 0;
        case WM_KILLFOCUS:
            for (auto& b : g.buttons) b.pressed = false;
            stopDrive();  // don't leave the robot driving if the window loses focus
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            stopDrive();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    g.hostW = L"192.168.4.1";
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--host" && i + 1 < argc) g.hostW = argv[++i];
        else if (a == L"--port" && i + 1 < argc) g.port = _wtoi(argv[++i]);
    }
    if (argv) LocalFree(argv);
    g.hostA.assign(g.hostW.begin(), g.hostW.end());

    buildButtons();
    WifiLink link(g.hostW, g.port);
    g.link = &link;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MilaController";
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;  // fixed size
    RECT rc = {0, 0, W, H};
    AdjustWindowRect(&rc, style, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"MILA Control", style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g.link = nullptr;
    return 0;
}

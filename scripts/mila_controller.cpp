// MILA WiFi controller (native Win32) — C++ port of mila_controller.py.
// Mirrors the onboard web dashboard. No third-party dependencies: GDI for UI,
// WinHTTP for transport, XInput for gamepads.
//
// USAGE
//   mila_controller.exe
//   mila_controller.exe --host 192.168.4.1 --port 5010
//   mila_controller.exe --host auto          (scan the network for MILA)
//
//   Host/port/mode are remembered in %APPDATA%\MILA\controller.ini; command
//   line flags override them.
//
//   If MILA joined NORA's network (fleet mode) instead of hosting her own
//   AP, she won't be at 192.168.4.1 anymore — press SCAN (or use --host auto)
//   to find her, or check NORA's registry (http://192.168.4.1:5000/robots).
//
// BUILD
//   cmake -B build -G "MinGW Makefiles" && cmake --build build
//   or: g++ -std=c++17 -O2 -mwindows -static mila_controller.cpp -o mila_controller.exe
//           -lwinhttp -lgdi32 -luser32 -lshell32 -lws2_32
//
// CONTROLS
//   1 / 2 / 3     WASD / TANK / OBSTACLE mode
//   WASD mode:    Arrow keys or WASD to drive, Space to stop
//   TANK mode:    Q/A = left track fwd/back, E/D = right track fwd/back
//   Speed:        drag the slider for an exact value, SPD -/+ step by 25
//   Gamepad:      left stick / D-pad drive (TANK: left+right stick = tracks),
//                 RT/LT raise/lower speed, A = stop, LB/RB = change mode
//   Esc           quit
//   Everything is also clickable with the mouse.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

static const double NaN = std::numeric_limits<double>::quiet_NaN();

static std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string narrow(const std::wstring& s) { return std::string(s.begin(), s.end()); }

static double toNum(const std::string& s) {
    char* e = nullptr;
    double v = std::strtod(s.c_str(), &e);
    return (e == s.c_str()) ? NaN : v;
}

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

static bool httpGet(HINTERNET session, const std::wstring& host, int port, const std::string& path,
                    std::string* body) {
    if (!session) return false;
    bool success = false;
    std::wstring wpath = widen(path);
    HINTERNET conn = WinHttpConnect(session, host.c_str(), (INTERNET_PORT)port, 0);
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

// One telemetry reading, kept for the history graphs and CSV export.
struct Sample {
    double t = 0;  // seconds since epoch
    double dist = NaN, left = NaN, right = NaN, temp = NaN, hum = NaN, speed = NaN;
    std::string mode, cmd;
};

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

    void retarget(const std::wstring& host, int port) {
        std::lock_guard<std::mutex> lk(lock_);
        host_ = host;
        port_ = port;
        data_.clear();
        ok_ = false;
    }

    std::vector<Sample> tail(size_t n) {
        std::lock_guard<std::mutex> lk(lock_);
        size_t from = history_.size() > n ? history_.size() - n : 0;
        return std::vector<Sample>(history_.begin() + from, history_.end());
    }

    std::vector<Sample> all() {
        std::lock_guard<std::mutex> lk(lock_);
        return history_;
    }

    // Commands are fire-and-forget; each runs on its own thread like before.
    void cmd(const std::string& action) { fireAndForget("/cmd?v=" + action); }
    void mode(const std::string& m) { fireAndForget("/mode?v=" + m); }

private:
    bool get(const std::string& path, std::string* body) {
        std::wstring host;
        int port;
        {
            std::lock_guard<std::mutex> lk(lock_);
            host = host_;
            port = port_;
        }
        return httpGet(session_, host, port, path, body);
    }

    void fireAndForget(const std::string& path) {
        std::thread([this, path] { get(path, nullptr); }).detach();
    }

    static Sample makeSample(const std::map<std::string, std::string>& d) {
        auto val = [&](const char* k) {
            auto it = d.find(k);
            return it == d.end() ? std::string() : it->second;
        };
        Sample s;
        s.t = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        s.dist = toNum(val("dist"));
        s.left = toNum(val("left"));
        s.right = toNum(val("right"));
        s.temp = toNum(val("temp"));
        s.hum = toNum(val("hum"));
        s.speed = toNum(val("speed"));
        s.mode = val("mode");
        s.cmd = val("cmd");
        return s;
    }

    void poll() {
        while (running_) {
            std::string body;
            if (get("/status", &body)) {
                auto parsed = parseFlatJson(body);
                std::lock_guard<std::mutex> lk(lock_);
                data_ = parsed;
                history_.push_back(makeSample(parsed));
                if (history_.size() > 200000) history_.erase(history_.begin(), history_.begin() + 1000);
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
    std::vector<Sample> history_;
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
static const COLORREF AMBER     = RGB(255, 190, 60);
static const COLORREF GREEN     = RGB(0, 255, 136);
static const COLORREF TEXT      = RGB(224, 224, 240);
static const COLORREF DIM       = RGB(102, 102, 136);
static const COLORREF ACCENT_BG = RGB(14, 34, 46);

static const int W = 840, H = 720;  // left 420px = controls, right 420px = radar / graphs / connection
static const UINT WM_FOUND = WM_APP + 1;

struct Button {
    RECT rect;
    std::wstring label;
    std::function<std::wstring()> labelFn;  // optional dynamic label
    std::function<bool()> vis;              // optional visibility override
    std::function<void()> cb;
    bool active = false;
    bool pressed = false;
    int group = 0;  // 0 always, 1 WASD-only, 2 TANK-only

    bool hit(POINT p) const { return PtInRect(&rect, p) != 0; }
};

enum Group { ALWAYS = 0, WASD_ONLY = 1, TANK_ONLY = 2 };

// Speed slider geometry (left column).
static const int SL_L = 80, SL_R = 340, SL_Y = 172;

struct App {
    HWND hwnd = nullptr;
    std::wstring hostW = L"192.168.4.1";
    std::string hostA = "192.168.4.1";
    int port = 5010;
    WifiLink* link = nullptr;
    HINTERNET scanSession = nullptr;

    std::string mode = "wasd";  // mirrors the firmware's WASD boot default
    bool modeFromSettings = false;
    bool modeSynced = false;
    std::string activeDrive;
    int speedPct = 100;
    bool sliderDrag = false;
    bool speedPending = false;
    DWORD speedHold = 0, lastSpeedSend = 0;

    std::vector<Button> buttons;
    size_t modeBtn[3] = {0, 0, 0};

    // discovery
    std::atomic<bool> scanning{false};
    std::atomic<bool> autoConnect{false};
    std::atomic<bool> autoDone{false};
    std::atomic<size_t> scanDone{0}, scanTotal{0};
    std::mutex scanMu;
    std::vector<std::string> found;
    bool scanEver = false;

    // gamepad
    int padIndex = -1;
    DWORD padNextProbe = 0, padLastTick = 0;
    WORD padPrevButtons = 0;
    std::string padDrive;
    int padL = 0, padR = 0;
    double padSpeedF = 100;

    std::string exportMsg;

    HFONT fBig = nullptr, fMed = nullptr, fSml = nullptr;
    HBITMAP backBmp = nullptr;
    HDC backDc = nullptr;
};

static App g;

// ---- settings (%APPDATA%\MILA\controller.ini) ----

static std::wstring settingsPath(bool create) {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return L"";
    std::wstring dir = std::wstring(buf) + L"\\MILA";
    if (create) CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\controller.ini";
}

static void loadSettings() {
    std::wstring p = settingsPath(false);
    if (p.empty()) return;
    FILE* f = _wfopen(p.c_str(), L"r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == "host" && !v.empty()) { g.hostA = v; g.hostW = widen(v); }
        else if (k == "port" && atoi(v.c_str()) > 0) g.port = atoi(v.c_str());
        else if (k == "mode" && (v == "wasd" || v == "tank" || v == "obstacle")) { g.mode = v; g.modeFromSettings = true; }
    }
    fclose(f);
}

static void saveSettings() {
    std::wstring p = settingsPath(true);
    if (p.empty()) return;
    FILE* f = _wfopen(p.c_str(), L"w");
    if (!f) return;
    fprintf(f, "host=%s\nport=%d\nmode=%s\n", g.hostA.c_str(), g.port, g.mode.c_str());
    fclose(f);
}

// ---- actions ----

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

static void applyModeButtons() {
    const char* names[3] = {"wasd", "tank", "obstacle"};
    for (int i = 0; i < 3; ++i) g.buttons[g.modeBtn[i]].active = (g.mode == names[i]);
}

static void setMode(const std::string& m) {
    g.mode = m;
    g.link->mode(m);
    applyModeButtons();
    saveSettings();
}

// Local speed change: shows immediately and is sent (throttled) by flushSpeed().
static void setSpeedLocal(int v) {
    v = std::max(25, std::min(100, v));
    if (v == g.speedPct && !g.speedPending) return;
    g.speedPct = v;
    g.speedPending = true;
    g.speedHold = GetTickCount() + 1500;  // don't let stale telemetry yank the slider back
}

static void flushSpeed() {
    DWORD now = GetTickCount();
    if (g.speedPending && now - g.lastSpeedSend >= 80) {
        g.speedPending = false;
        g.lastSpeedSend = now;
        g.link->cmd("SPEED:" + std::to_string(g.speedPct));
    }
}

static void connectTo(const std::string& host) {
    g.hostA = host;
    g.hostW = widen(host);
    g.link->retarget(g.hostW, g.port);
    g.modeSynced = false;
    saveSettings();
}

// ---- discovery ----

static std::vector<std::string> localSubnets() {
    std::vector<std::string> out;
    char name[256];
    if (gethostname(name, sizeof name) != 0) return out;
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &res) != 0) return out;
    for (addrinfo* p = res; p; p = p->ai_next) {
        auto* a = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&a->sin_addr);
        if (b[0] == 127 || (b[0] == 169 && b[1] == 254)) continue;
        char buf[32];
        snprintf(buf, sizeof buf, "%d.%d.%d.", b[0], b[1], b[2]);
        if (std::find(out.begin(), out.end(), buf) == out.end()) out.push_back(buf);
    }
    freeaddrinfo(res);
    return out;
}

// Fast reachability check: a non-blocking connect with a hard timeout. WinHTTP is far too patient with
// dead addresses, which makes a /24 sweep take ~45 s; this brings it down to a couple of seconds.
static bool tcpOpen(const std::string& host, int port, int timeoutMs) {
    addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    bool ok = false;
    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s != INVALID_SOCKET) {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        if (connect(s, res->ai_addr, (int)res->ai_addrlen) == 0) {
            ok = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set w, e;
            FD_ZERO(&w);
            FD_ZERO(&e);
            FD_SET(s, &w);
            FD_SET(s, &e);
            timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
            if (select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) {
                int err = 0, len = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                ok = (err == 0);
            }
        }
        closesocket(s);
    }
    freeaddrinfo(res);
    return ok;
}

static bool probeMila(const std::string& host, int port) {
    if (!tcpOpen(host, port, 300)) return false;
    std::string body;
    if (!httpGet(g.scanSession, widen(host), port, "/status", &body)) return false;
    return body.find("\"mode\"") != std::string::npos && body.find("\"dist\"") != std::string::npos;
}

static void startScan(bool autoConnect) {
    if (g.scanning.exchange(true)) return;
    g.scanEver = true;
    g.scanDone = 0;
    g.scanTotal = 0;
    g.autoConnect = autoConnect;
    g.autoDone = false;
    {
        std::lock_guard<std::mutex> lk(g.scanMu);
        g.found.clear();
    }
    int port = g.port;
    std::thread([port] {
        std::vector<std::string> cands;
        std::set<std::string> seen;
        auto add = [&](const std::string& s) {
            if (seen.insert(s).second) cands.push_back(s);
        };

        // Robots registered with NORA (best effort: pull every IPv4 out of the reply).
        std::string reg;
        if (httpGet(g.scanSession, L"192.168.4.1", 5000, "/robots", &reg)) {
            std::regex ip(R"((\d{1,3}\.){3}\d{1,3})");
            for (auto it = std::sregex_iterator(reg.begin(), reg.end(), ip); it != std::sregex_iterator(); ++it)
                add(it->str());
        }
        add("127.0.0.1");   // a simulator running on this machine (sim/MockMila.cs)
        add("mila.local");  // the firmware advertises itself over mDNS
        add("192.168.4.1"); // MILA's own access point
        std::vector<std::string> prefixes = localSubnets();
        prefixes.push_back("192.168.4.");
        for (const auto& pre : prefixes)
            for (int i = 1; i <= 254; ++i) add(pre + std::to_string(i));

        g.scanTotal = cands.size();
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (;;) {
                size_t i = next++;
                if (i >= cands.size()) break;
                if (probeMila(cands[i], port)) {
                    {
                        std::lock_guard<std::mutex> lk(g.scanMu);
                        g.found.push_back(cands[i]);
                    }
                    if (g.autoConnect && !g.autoDone.exchange(true)) PostMessage(g.hwnd, WM_FOUND, 0, 0);
                }
                ++g.scanDone;
            }
        };
        std::vector<std::thread> pool;
        for (int i = 0; i < 64; ++i) pool.emplace_back(worker);
        for (auto& t : pool) t.join();
        g.scanning = false;
    }).detach();
}

// ---- CSV export ----

static void exportCsv() {
    auto samples = g.link->all();
    if (samples.empty()) {
        g.exportMsg = "no samples yet";
        return;
    }
    wchar_t docs[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) {
        g.exportMsg = "no Documents folder";
        return;
    }
    std::wstring dir = std::wstring(docs) + L"\\MILA-telemetry";
    CreateDirectoryW(dir.c_str(), nullptr);

    time_t now = time(nullptr);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", localtime(&now));
    std::string fname = std::string("telemetry_") + stamp + ".csv";
    std::wstring path = dir + L"\\" + widen(fname);

    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) {
        g.exportMsg = "could not write file";
        return;
    }
    fprintf(f, "timestamp,dist_cm,left_cm,right_cm,temp_c,hum_pct,speed_pct,mode,last_cmd\n");
    auto num = [](double v) {
        char b[32];
        if (std::isnan(v)) return std::string();
        snprintf(b, sizeof b, "%g", v);
        return std::string(b);
    };
    for (const auto& s : samples) {
        time_t secs = (time_t)s.t;
        char ts[32];
        strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&secs));
        fprintf(f, "%s.%03d,%s,%s,%s,%s,%s,%s,%s,%s\n", ts, (int)((s.t - std::floor(s.t)) * 1000), num(s.dist).c_str(),
                num(s.left).c_str(), num(s.right).c_str(), num(s.temp).c_str(), num(s.hum).c_str(),
                num(s.speed).c_str(), s.mode.c_str(), s.cmd.c_str());
    }
    fclose(f);
    g.exportMsg = "saved " + std::to_string(samples.size()) + " rows: " + fname;
}

// ---- gamepad (XInput) ----

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
static XInputGetStateFn pXInputGetState = nullptr;

static void initXInput() {
    const wchar_t* dlls[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* d : dlls) {
        HMODULE m = LoadLibraryW(d);
        if (!m) continue;
        pXInputGetState = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
        if (pXInputGetState) return;
    }
}

static void cycleMode(int dir) {
    const char* names[3] = {"wasd", "tank", "obstacle"};
    int cur = 0;
    for (int i = 0; i < 3; ++i)
        if (g.mode == names[i]) cur = i;
    setMode(names[(cur + dir + 3) % 3]);
}

static void pollPad() {
    if (!pXInputGetState) return;
    DWORD now = GetTickCount();
    XINPUT_STATE st = {};
    if (g.padIndex < 0) {
        if (now < g.padNextProbe) return;
        g.padNextProbe = now + 1000;  // probing empty slots is slow, so only once a second
        for (int i = 0; i < 4; ++i) {
            if (pXInputGetState(i, &st) == ERROR_SUCCESS) {
                g.padIndex = i;
                g.padLastTick = now;
                break;
            }
        }
        if (g.padIndex < 0) return;
    }
    if (pXInputGetState(g.padIndex, &st) != ERROR_SUCCESS) {
        // Unplugged mid-drive: stop the robot.
        g.padIndex = -1;
        g.padDrive.clear();
        g.padL = g.padR = 0;
        stopDrive();
        return;
    }

    const XINPUT_GAMEPAD& p = st.Gamepad;
    const double dz = 0.45;
    auto axis = [](SHORT v) { return std::max(-1.0, v / 32767.0); };
    double lx = axis(p.sThumbLX), ly = axis(p.sThumbLY), ry = axis(p.sThumbRY);
    double dt = (now - g.padLastTick) / 1000.0;
    g.padLastTick = now;

    // A = stop, LB/RB = previous/next mode (edge-triggered).
    WORD pressed = p.wButtons & ~g.padPrevButtons;
    g.padPrevButtons = p.wButtons;
    if (pressed & XINPUT_GAMEPAD_A) {
        g.padDrive.clear();
        g.padL = g.padR = 0;
        g.activeDrive.clear();
        g.link->cmd("STOP");
    }
    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) cycleMode(-1);
    if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) cycleMode(+1);

    if (g.mode == "wasd") {
        std::string want;
        if (std::hypot(lx, ly) >= dz) {
            if (std::fabs(ly) >= std::fabs(lx)) want = ly > 0 ? "FORWARD" : "BACKWARD";
            else want = lx > 0 ? "RIGHT" : "LEFT";
        } else if (p.wButtons & XINPUT_GAMEPAD_DPAD_UP) want = "FORWARD";
        else if (p.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) want = "BACKWARD";
        else if (p.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) want = "LEFT";
        else if (p.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) want = "RIGHT";
        if (want != g.padDrive) {
            g.padDrive = want;
            if (want.empty()) stopDrive();
            else startDrive(want);
        }
    } else if (g.mode == "tank") {
        // Left stick Y = left track, right stick Y = right track.
        int l = ly >= dz ? 1 : (ly <= -dz ? -1 : 0);
        int r = ry >= dz ? 1 : (ry <= -dz ? -1 : 0);
        if (l != g.padL || r != g.padR) {
            if (l == 0 && r == 0) {
                g.activeDrive.clear();
                g.link->cmd("STOP");
            } else {
                // STOP is global, so when a track lets go, stop and re-issue the other.
                bool released = (g.padL != 0 && l == 0) || (g.padR != 0 && r == 0);
                if (released) g.link->cmd("STOP");
                if (l != 0 && (released || l != g.padL)) g.link->cmd(l > 0 ? "L_FWD" : "L_BWD");
                if (r != 0 && (released || r != g.padR)) g.link->cmd(r > 0 ? "R_FWD" : "R_BWD");
                g.activeDrive = "PAD_TANK";  // sentinel so Space / focus loss still sends STOP
            }
            g.padL = l;
            g.padR = r;
        }
    } else {
        g.padDrive.clear();
        g.padL = g.padR = 0;
    }

    // Triggers ramp speed: RT up, LT down (full pull = 60 %/s).
    double rt = p.bRightTrigger / 255.0, lt = p.bLeftTrigger / 255.0;
    if (rt > 0.1 || lt > 0.1) {
        g.padSpeedF = std::max(25.0, std::min(100.0, g.padSpeedF + (rt - lt) * 60.0 * dt));
        setSpeedLocal((int)std::lround(g.padSpeedF));
    } else {
        g.padSpeedF = g.speedPct;
    }
}

// ---- layout ----

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
    g.modeBtn[0] = addButton(20, 100, 120, 40, L"WASD", [] { setMode("wasd"); });
    g.modeBtn[1] = addButton(150, 100, 120, 40, L"TANK", [] { setMode("tank"); });
    g.modeBtn[2] = addButton(280, 100, 120, 40, L"OBSTACLE", [] { setMode("obstacle"); });
    applyModeButtons();

    // Speed: [-]  slider  [+]. Presets mirror the firmware's speedPresets[] (100/75/50/25).
    addButton(20, 150, 44, 40, L"-", [] { setSpeedLocal(g.speedPct - 25); });
    addButton(356, 150, 44, 40, L"+", [] { setSpeedLocal(g.speedPct + 25); });

    // WASD d-pad
    const int cx = 210, cy = 280, bs = 70, gap = 8;
    addButton(cx - bs / 2, cy - bs - gap, bs, bs, L"^", [] { startDrive("FORWARD"); }, WASD_ONLY);
    addButton(cx - bs / 2, cy + gap, bs, bs, L"v", [] { startDrive("BACKWARD"); }, WASD_ONLY);
    addButton(cx - bs - gap - bs / 2 - gap, cy - bs / 2, bs, bs, L"<", [] { startDrive("LEFT"); }, WASD_ONLY);
    addButton(cx + gap + bs / 2 + gap, cy - bs / 2, bs, bs, L">", [] { startDrive("RIGHT"); }, WASD_ONLY);
    addButton(cx - bs / 2, cy - bs / 2, bs, bs, L"STOP", [] { g.link->cmd("STOP"); }, WASD_ONLY);

    // TANK track buttons
    const int ty = 280;
    addButton(60, ty - 40, 90, 50, L"Q\nL-FWD", [] { startDrive("L_FWD"); }, TANK_ONLY);
    addButton(60, ty + 40, 90, 50, L"A\nL-BWD", [] { startDrive("L_BWD"); }, TANK_ONLY);
    addButton(420 - 150, ty - 40, 90, 50, L"E\nR-FWD", [] { startDrive("R_FWD"); }, TANK_ONLY);
    addButton(420 - 150, ty + 40, 90, 50, L"D\nR-BWD", [] { startDrive("R_BWD"); }, TANK_ONLY);

    // Right column: CSV export, robot discovery.
    addButton(440, 545, 180, 36, L"EXPORT CSV", [] { exportCsv(); });

    size_t scanBtn = addButton(440, 622, 150, 34, L"SCAN", [] { startScan(false); });
    g.buttons[scanBtn].labelFn = [] {
        if (!g.scanning) return std::wstring(L"SCAN NETWORK");
        size_t total = g.scanTotal;
        int pct = total ? (int)(100 * g.scanDone / total) : 0;
        return L"SCANNING " + std::to_wstring(pct) + L"%";
    };
    for (int i = 0; i < 3; ++i) {
        size_t s = addButton(440 + i * 130, 668, 122, 34, L"", [i] {
            std::string ip;
            {
                std::lock_guard<std::mutex> lk(g.scanMu);
                if ((size_t)i < g.found.size()) ip = g.found[i];
            }
            if (!ip.empty()) connectTo(ip);
        });
        g.buttons[s].labelFn = [i] {
            std::lock_guard<std::mutex> lk(g.scanMu);
            return (size_t)i < g.found.size() ? widen(g.found[i]) : std::wstring();
        };
        g.buttons[s].vis = [i] {
            std::lock_guard<std::mutex> lk(g.scanMu);
            return (size_t)i < g.found.size();
        };
    }
}

static bool visible(const Button& b) {
    if (b.vis) return b.vis();
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
    else if (fmt & DT_RIGHT) r = {x - 1000, y, x, y + 100};
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}

static void drawButton(HDC dc, const Button& b) {
    bool on = b.active || b.pressed;
    fillRound(dc, b.rect, on ? ACCENT_BG : PANEL, on ? ACCENT : BORDER, 2, 8);
    SelectObject(dc, g.fMed);
    SetTextColor(dc, on ? ACCENT : TEXT);
    RECT r = b.rect;
    std::wstring label = b.labelFn ? b.labelFn() : b.label;
    size_t nl = label.find(L'\n');
    if (nl == std::wstring::npos) {
        DrawTextW(dc, label.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else {
        std::wstring a = label.substr(0, nl), c = label.substr(nl + 1);
        RECT r1 = r, r2 = r;
        r1.bottom = (r.top + r.bottom) / 2;
        r2.top = r1.bottom;
        DrawTextW(dc, a.c_str(), -1, &r1, DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        DrawTextW(dc, c.c_str(), -1, &r2, DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
}

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

static COLORREF mix(COLORREF a, COLORREF b, double t) {
    auto ch = [&](int (*get)(COLORREF)) { return (int)std::lround(get(a) * t + get(b) * (1 - t)); };
    return RGB(ch([](COLORREF c) -> int { return GetRValue(c); }), ch([](COLORREF c) -> int { return GetGValue(c); }),
               ch([](COLORREF c) -> int { return GetBValue(c); }));
}

static COLORREF proxColor(double cm) { return cm < 20 ? RED : (cm < 50 ? AMBER : GREEN); }

static std::wstring fmtNum(double v, const wchar_t* fmt = L"%.0f") {
    if (std::isnan(v)) return L"--";
    wchar_t b[32];
    swprintf(b, 32, fmt, v);
    return b;
}

// Radar: three wedges (left / front / right) whose length is the distance, coloured by proximity.
static void drawRadar(HDC dc, const RECT& panel, double left, double front, double right, bool live) {
    fillRound(dc, panel, PANEL, BORDER, 1, 8);
    drawText(dc, g.fSml, DIM, L"PROXIMITY", panel.left + 12, panel.top + 8);

    const double maxRange = 100.0, PI = 3.14159265358979;
    int cx = (panel.left + panel.right) / 2, cy = panel.bottom - 36, R = 170;
    auto pt = [&](double deg, int r, int& x, int& y) {
        double a = deg * PI / 180.0;
        x = cx + (int)std::lround(r * std::cos(a));
        y = cy - (int)std::lround(r * std::sin(a));
    };

    // range rings
    HPEN ring = CreatePen(PS_SOLID, 1, BORDER);
    HGDIOBJ op = SelectObject(dc, ring), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (int i = 1; i <= 4; ++i) {
        int r = R * i / 4, sx, sy, ex, ey;
        pt(0, r, ex, ey);
        pt(180, r, sx, sy);
        Arc(dc, cx - r, cy - r, cx + r, cy + r, ex, ey, sx, sy);  // counter-clockwise: right -> left over the top
    }
    MoveToEx(dc, cx - R, cy, nullptr);
    LineTo(dc, cx + R, cy);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(ring);

    struct Wedge { double a1, a2, v; const wchar_t* name; int labelX; };
    Wedge ws[3] = {{125, 165, left, L"LEFT", panel.left + 80},
                   {70, 110, front, L"FRONT", cx},
                   {15, 55, right, L"RIGHT", panel.right - 80}};
    for (const auto& w : ws) {
        COLORREF col = std::isnan(w.v) ? DIM : proxColor(w.v);
        if (!live) col = DIM;
        if (!std::isnan(w.v)) {
            int r = (int)std::lround(R * std::max(0.06, std::min(1.0, w.v / maxRange)));
            int sx, sy, ex, ey;
            pt(w.a1, r, sx, sy);
            pt(w.a2, r, ex, ey);
            HBRUSH br = CreateSolidBrush(mix(col, PANEL, 0.35));
            HPEN pen = CreatePen(PS_SOLID, 2, col);
            HGDIOBJ obr = SelectObject(dc, br), open = SelectObject(dc, pen);
            Pie(dc, cx - r, cy - r, cx + r, cy + r, sx, sy, ex, ey);
            SelectObject(dc, obr);
            SelectObject(dc, open);
            DeleteObject(br);
            DeleteObject(pen);
        }
        drawText(dc, g.fSml, DIM, w.name, w.labelX, cy + 6, DT_CENTER);
        drawText(dc, g.fMed, col, fmtNum(w.v) + (std::isnan(w.v) ? L"" : L" cm"), w.labelX, cy + 18, DT_CENTER);
    }
    // the robot
    POINT tri[3] = {{cx, cy - 10}, {cx - 7, cy + 2}, {cx + 7, cy + 2}};
    HBRUSH tb = CreateSolidBrush(ACCENT);
    HGDIOBJ otb = SelectObject(dc, tb), onp = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, tri, 3);
    SelectObject(dc, otb);
    SelectObject(dc, onp);
    DeleteObject(tb);
}

struct Series {
    std::vector<double> v;
    COLORREF col;
};

// Line chart over the last N samples. Each series is scaled to lo..hi (0 = auto).
static void drawGraph(HDC dc, const RECT& box, const wchar_t* title, const std::vector<Series>& series,
                      double lo, double hi, const std::vector<std::wstring>& latest) {
    fillRound(dc, box, PANEL, BORDER, 1, 8);
    drawText(dc, g.fSml, DIM, title, box.left + 12, box.top + 7);
    int rx = box.right - 12;
    for (size_t i = series.size(); i-- > 0;) {
        SelectObject(dc, g.fMed);
        SIZE sz;
        GetTextExtentPoint32W(dc, latest[i].c_str(), (int)latest[i].size(), &sz);
        drawText(dc, g.fMed, series[i].col, latest[i], rx, box.top + 5, DT_RIGHT);
        rx -= sz.cx + 14;
    }

    RECT plot = {box.left + 12, box.top + 28, box.right - 12, box.bottom - 10};
    HPEN grid = CreatePen(PS_SOLID, 1, BORDER);
    HGDIOBJ og = SelectObject(dc, grid);
    for (int i = 0; i <= 2; ++i) {
        int y = plot.top + (plot.bottom - plot.top) * i / 2;
        MoveToEx(dc, plot.left, y, nullptr);
        LineTo(dc, plot.right, y);
    }
    SelectObject(dc, og);
    DeleteObject(grid);

    for (const auto& s : series) {
        double mn = lo, mx = hi;
        if (mn == mx) {  // auto-scale this series with a little headroom
            mn = 1e300;
            mx = -1e300;
            for (double v : s.v)
                if (!std::isnan(v)) { mn = std::min(mn, v); mx = std::max(mx, v); }
            if (mn > mx) continue;
            if (mx - mn < 4) { double mid = (mx + mn) / 2; mn = mid - 2; mx = mid + 2; }
        }
        HPEN pen = CreatePen(PS_SOLID, 2, s.col);
        HGDIOBJ op = SelectObject(dc, pen);
        bool pen_down = false;
        size_t n = s.v.size();
        for (size_t i = 0; i < n; ++i) {
            if (std::isnan(s.v[i])) { pen_down = false; continue; }
            int x = plot.left + (int)((plot.right - plot.left) * (double)i / std::max<size_t>(1, 149));
            double f = std::max(0.0, std::min(1.0, (s.v[i] - mn) / (mx - mn)));
            int y = plot.bottom - (int)((plot.bottom - plot.top) * f);
            if (!pen_down) { MoveToEx(dc, x, y, nullptr); pen_down = true; }
            else LineTo(dc, x, y);
        }
        SelectObject(dc, op);
        DeleteObject(pen);
    }
}

static void render(HDC dc) {
    RECT full = {0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(BG);
    FillRect(dc, &full, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    auto t = g.link->telemetry();
    bool ok = g.link->ok();
    auto sp = t.find("speed");
    if (sp != t.end() && !g.sliderDrag && GetTickCount() > g.speedHold && !g.speedPending)
        g.speedPct = (int)std::strtod(sp->second.c_str(), nullptr);

    // ---- left column: controls ----
    drawText(dc, g.fBig, ACCENT, L"MILA", 210, 14, DT_CENTER);
    std::wstring sub = widen(g.hostA) + L":" + std::to_wstring(g.port) + L"   " + (ok ? L"connected" : L"OFFLINE");
    if (g.padIndex >= 0) sub += L"   [gamepad]";
    drawText(dc, g.fSml, ok ? GREEN : RED, sub, 210, 48, DT_CENTER);

    for (const auto& b : g.buttons)
        if (visible(b) && b.rect.left < 420) drawButton(dc, b);

    // speed slider
    {
        RECT track = {SL_L, SL_Y - 3, SL_R, SL_Y + 3};
        HBRUSH tb = CreateSolidBrush(BORDER);
        FillRect(dc, &track, tb);
        DeleteObject(tb);
        int kx = SL_L + (g.speedPct - 25) * (SL_R - SL_L) / 75;
        RECT fillR = {SL_L, SL_Y - 3, kx, SL_Y + 3};
        HBRUSH fb = CreateSolidBrush(ACCENT);
        FillRect(dc, &fillR, fb);
        DeleteObject(fb);
        HBRUSH kb = CreateSolidBrush(g.sliderDrag ? ACCENT : TEXT);
        HPEN kp = CreatePen(PS_SOLID, 2, ACCENT);
        HGDIOBJ okb = SelectObject(dc, kb), okp = SelectObject(dc, kp);
        Ellipse(dc, kx - 10, SL_Y - 10, kx + 10, SL_Y + 10);
        SelectObject(dc, okb);
        SelectObject(dc, okp);
        DeleteObject(kb);
        DeleteObject(kp);
        drawText(dc, g.fSml, DIM, L"SPEED " + std::to_wstring(g.speedPct) + L"%", (SL_L + SL_R) / 2, 150, DT_CENTER);
    }

    const int sy = 400, colw = (420 - 50) / 2, c2 = 30 + colw;
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

    drawText(dc, g.fSml, DIM, L"1/2/3 mode - WASD/QAED drive - space stop - esc quit", 210, H - 24, DT_CENTER);

    // ---- right column: radar, graphs, export, discovery ----
    RECT radar = {440, 20, 820, 262};
    drawRadar(dc, radar, toNum(tget(t, "left", "")), toNum(tget(t, "dist", "")), toNum(tget(t, "right", "")), ok);

    auto hist = g.link->tail(150);
    Series sDist{{}, ACCENT}, sTemp{{}, ACCENT}, sHum{{}, GREEN};
    for (const auto& s : hist) {
        sDist.v.push_back(s.dist);
        sTemp.v.push_back(s.temp);
        sHum.v.push_back(s.hum);
    }
    double lastDist = hist.empty() ? NaN : hist.back().dist;
    double lastTemp = hist.empty() ? NaN : hist.back().temp;
    double lastHum = hist.empty() ? NaN : hist.back().hum;
    double dmax = 100;
    for (double v : sDist.v)
        if (!std::isnan(v)) dmax = std::max(dmax, v);
    RECT g1 = {440, 272, 820, 392}, g2 = {440, 402, 820, 522};
    drawGraph(dc, g1, L"FRONT DISTANCE (LAST ~60 S)", {sDist}, 0, dmax, {fmtNum(lastDist, L"%.1f") + L" cm"});
    drawGraph(dc, g2, L"TEMP / HUMIDITY", {sTemp, sHum}, 0, 0,
              {fmtNum(lastTemp, L"%.1f") + L" C", fmtNum(lastHum, L"%.1f") + L" %"});

    for (const auto& b : g.buttons)
        if (visible(b) && b.rect.left >= 420) drawButton(dc, b);
    drawText(dc, g.fSml, DIM, widen(g.exportMsg), 632, 556);

    // discovery status
    std::wstring status;
    {
        std::lock_guard<std::mutex> lk(g.scanMu);
        if (g.scanning) status = L"looking for MILA on your network...";
        else if (g.scanEver) status = g.found.empty() ? L"no robots found" : L"click an address to connect";
        else status = L"scan to find MILA (or use --host auto)";
    }
    drawText(dc, g.fSml, DIM, L"ROBOT  " + widen(g.hostA) + L":" + std::to_wstring(g.port), 440, 600);
    drawText(dc, g.fSml, DIM, status, 600, 632);
}

// ---- window ----

static void setSliderFromX(int x) {
    int v = 25 + (int)std::lround((double)(x - SL_L) * 75.0 / (SL_R - SL_L));
    setSpeedLocal(v);
}

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
        case WM_TIMER: {
            pollPad();
            flushSpeed();
            // Once connected: push a remembered mode to the robot, or adopt the robot's current one.
            if (!g.modeSynced && g.link->ok()) {
                auto t = g.link->telemetry();
                auto it = t.find("mode");
                if (g.modeFromSettings) {
                    if (it == t.end() || it->second != g.mode) g.link->mode(g.mode);
                } else if (it != t.end() && (it->second == "wasd" || it->second == "tank" || it->second == "obstacle")) {
                    g.mode = it->second;
                    applyModeButtons();
                }
                g.modeSynced = true;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_FOUND: {
            std::string ip;
            {
                std::lock_guard<std::mutex> lk(g.scanMu);
                if (!g.found.empty()) ip = g.found[0];
            }
            if (!ip.empty()) connectTo(ip);
            return 0;
        }
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
            RECT sl = {SL_L - 14, SL_Y - 16, SL_R + 14, SL_Y + 16};
            if (PtInRect(&sl, p)) {
                g.sliderDrag = true;
                setSliderFromX(p.x);
                return 0;
            }
            for (auto& b : g.buttons) {
                if (visible(b) && b.hit(p)) {
                    b.pressed = true;
                    b.cb();
                    break;
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (g.sliderDrag) setSliderFromX((short)LOWORD(lp));
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            g.sliderDrag = false;
            for (auto& b : g.buttons) b.pressed = false;
            stopDrive();
            return 0;
        case WM_KILLFOCUS:
            g.sliderDrag = false;
            for (auto& b : g.buttons) b.pressed = false;
            stopDrive();  // don't leave the robot driving if the window loses focus
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            stopDrive();
            saveSettings();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    initXInput();

    loadSettings();
    bool autoHost = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--host" && i + 1 < argc) {
            std::wstring h = argv[++i];
            if (h == L"auto") autoHost = true;
            else { g.hostW = h; g.hostA = narrow(h); }
        } else if (a == L"--port" && i + 1 < argc) g.port = _wtoi(argv[++i]);
    }
    if (argv) LocalFree(argv);

    g.scanSession = WinHttpOpen(L"MILA-Controller/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (g.scanSession) WinHttpSetTimeouts(g.scanSession, 300, 400, 400, 700);

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
    g.hwnd = CreateWindowW(wc.lpszClassName, L"MILA Control", style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 1;
    ShowWindow(g.hwnd, show);

    if (autoHost) startScan(true);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g.link = nullptr;
    WSACleanup();
    return 0;
}

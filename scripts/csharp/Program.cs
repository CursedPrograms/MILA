// MILA WiFi controller (WinForms) — C# port of mila_controller.py.
// Mirrors the onboard web dashboard. No NuGet dependencies (XInput via P/Invoke).
//
// USAGE
//   MilaController.exe
//   MilaController.exe --host 192.168.4.1 --port 5010
//   MilaController.exe --host auto          (scan the network for MILA)
//
//   Host/port/mode are remembered in %APPDATA%\MILA\controller.ini (shared with
//   the C++ controller); command line flags override them.
//
//   If MILA joined NORA's network (fleet mode) instead of hosting her own
//   AP, she won't be at 192.168.4.1 anymore — press SCAN NETWORK (or use
//   --host auto) to find her, or check NORA's registry
//   (http://192.168.4.1:5000/robots).
//
// BUILD
//   dotnet build -c Release            (see MilaController.csproj)
//   dotnet publish -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
//
// CONTROLS
//   1 / 2 / 3     WASD / TANK / OBSTACLE mode
//   WASD mode:    Arrow keys or WASD to drive, Space to stop
//   TANK mode:    Q/A = left track fwd/back, E/D = right track fwd/back
//   Speed:        drag the slider for an exact value, - / + step by 25
//   Gamepad:      left stick / D-pad drive (TANK: left+right stick = tracks),
//                 RT/LT raise/lower speed, A = stop, LB/RB = change mode
//   Esc           quit
//   Everything is also clickable with the mouse.

using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Http;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Windows.Forms;

namespace Mila
{
    // ------------------------------------------------------------------------
    // Transport
    // ------------------------------------------------------------------------

    // One telemetry reading, kept for the history graphs and CSV export.
    class Sample
    {
        public DateTime Time;
        public double Dist = double.NaN, Left = double.NaN, Right = double.NaN;
        public double Temp = double.NaN, Hum = double.NaN, Speed = double.NaN;
        public string Mode = "", Cmd = "";
    }

    class WifiLink : IDisposable
    {
        string host;
        int port;
        readonly HttpClient http;
        readonly object lk = new object();
        readonly BlockingCollection<string> queue = new BlockingCollection<string>();
        readonly List<Sample> history = new List<Sample>();
        Dictionary<string, string> data = new Dictionary<string, string>();
        volatile bool ok;
        volatile bool running = true;

        public WifiLink(string host, int port)
        {
            this.host = host;
            this.port = port;
            http = new HttpClient();
            http.Timeout = TimeSpan.FromSeconds(1);

            Thread poller = new Thread(Poll);
            poller.IsBackground = true;
            poller.Start();

            // One worker keeps commands in order (a STOP must never overtake
            // the FORWARD that preceded it).
            Thread worker = new Thread(Work);
            worker.IsBackground = true;
            worker.Start();
        }

        public bool Ok { get { return ok; } }

        string BaseUrl()
        {
            lock (lk) { return "http://" + host + ":" + port; }
        }

        public Dictionary<string, string> Telemetry()
        {
            lock (lk) { return new Dictionary<string, string>(data); }
        }

        public void Retarget(string newHost, int newPort)
        {
            lock (lk)
            {
                host = newHost;
                port = newPort;
                data = new Dictionary<string, string>();
                ok = false;
            }
        }

        public List<Sample> Tail(int n)
        {
            lock (lk)
            {
                int from = Math.Max(0, history.Count - n);
                return history.GetRange(from, history.Count - from);
            }
        }

        public List<Sample> All()
        {
            lock (lk) { return new List<Sample>(history); }
        }

        public void Cmd(string action) { queue.Add("/cmd?v=" + action); }
        public void Mode(string m) { queue.Add("/mode?v=" + m); }

        // Synchronous STOP for shutdown, when the worker may not get to run.
        public void StopNow()
        {
            try { http.GetStringAsync(BaseUrl() + "/cmd?v=STOP").Wait(); } catch (Exception) { }
        }

        // Does something at host:port look like a MILA? (used by network discovery)
        public static bool Probe(string host, int port)
        {
            if (!TcpOpen(host, port, 300)) return false;
            try
            {
                using (HttpClient c = new HttpClient())
                {
                    c.Timeout = TimeSpan.FromMilliseconds(800);
                    string body = c.GetStringAsync("http://" + host + ":" + port + "/status").Result;
                    return body.Contains("\"mode\"") && body.Contains("\"dist\"");
                }
            }
            catch (Exception) { return false; }
        }

        // Non-blocking connect with a hard timeout, so sweeping a /24 stays fast.
        public static bool TcpOpen(string host, int port, int ms)
        {
            try
            {
                using (TcpClient c = new TcpClient())
                {
                    IAsyncResult r = c.BeginConnect(host, port, null, null);
                    if (!r.AsyncWaitHandle.WaitOne(ms)) return false;
                    c.EndConnect(r);
                    return c.Connected;
                }
            }
            catch (Exception) { return false; }
        }

        public static string HttpGetString(string url, int timeoutMs)
        {
            try
            {
                using (HttpClient c = new HttpClient())
                {
                    c.Timeout = TimeSpan.FromMilliseconds(timeoutMs);
                    return c.GetStringAsync(url).Result;
                }
            }
            catch (Exception) { return null; }
        }

        void Work()
        {
            foreach (string path in queue.GetConsumingEnumerable())
            {
                try { http.GetStringAsync(BaseUrl() + path).Wait(); }
                catch (Exception)
                {
                    // Offline: drop the backlog rather than replaying stale drive
                    // commands when the link comes back.
                    string dropped;
                    while (queue.TryTake(out dropped)) { }
                }
            }
        }

        void Poll()
        {
            while (running)
            {
                try
                {
                    string body = http.GetStringAsync(BaseUrl() + "/status").Result;
                    Dictionary<string, string> parsed = ParseFlatJson(body);
                    lock (lk)
                    {
                        data = parsed;
                        history.Add(MakeSample(parsed));
                        if (history.Count > 200000) history.RemoveRange(0, 1000);
                    }
                    ok = true;
                }
                catch (Exception) { ok = false; }
                Thread.Sleep(400);
            }
        }

        static double Num(Dictionary<string, string> d, string k)
        {
            string s;
            double v;
            if (d.TryGetValue(k, out s) && double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v))
                return v;
            return double.NaN;
        }

        static string Str(Dictionary<string, string> d, string k)
        {
            string s;
            return d.TryGetValue(k, out s) ? s : "";
        }

        static Sample MakeSample(Dictionary<string, string> d)
        {
            Sample s = new Sample();
            s.Time = DateTime.Now;
            s.Dist = Num(d, "dist");
            s.Left = Num(d, "left");
            s.Right = Num(d, "right");
            s.Temp = Num(d, "temp");
            s.Hum = Num(d, "hum");
            s.Speed = Num(d, "speed");
            s.Mode = Str(d, "mode");
            s.Cmd = Str(d, "cmd");
            return s;
        }

        // Minimal parser for the firmware's flat /status JSON.
        static Dictionary<string, string> ParseFlatJson(string s)
        {
            Dictionary<string, string> o = new Dictionary<string, string>();
            int i = 0, n = s.Length;
            while (i < n)
            {
                while (i < n && s[i] != '"') i++;
                if (i >= n) break;
                string key = ReadString(s, ref i);
                while (i < n && s[i] != ':') i++;
                i++;
                while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
                string val;
                if (i < n && s[i] == '"') val = ReadString(s, ref i);
                else
                {
                    int start = i;
                    while (i < n && s[i] != ',' && s[i] != '}') i++;
                    val = s.Substring(start, i - start).Trim();
                }
                o[key] = val;
            }
            return o;
        }

        static string ReadString(string s, ref int i)
        {
            StringBuilder sb = new StringBuilder();
            i++;
            while (i < s.Length && s[i] != '"')
            {
                if (s[i] == '\\' && i + 1 < s.Length) i++;
                sb.Append(s[i++]);
            }
            i++;
            return sb.ToString();
        }

        public void Dispose()
        {
            running = false;
            queue.CompleteAdding();
            http.Dispose();
        }
    }

    // ------------------------------------------------------------------------
    // Gamepad (XInput)
    // ------------------------------------------------------------------------

    [StructLayout(LayoutKind.Sequential)]
    struct XInputGamepad
    {
        public ushort Buttons;
        public byte LeftTrigger, RightTrigger;
        public short ThumbLX, ThumbLY, ThumbRX, ThumbRY;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct XInputState
    {
        public uint PacketNumber;
        public XInputGamepad Gamepad;
    }

    static class XInput
    {
        public const ushort DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008;
        public const ushort LB = 0x0100, RB = 0x0200, A = 0x1000;

        [DllImport("xinput1_4.dll", EntryPoint = "XInputGetState")]
        static extern int Get14(int index, out XInputState state);
        [DllImport("xinput9_1_0.dll", EntryPoint = "XInputGetState")]
        static extern int Get910(int index, out XInputState state);

        static int dll = 0;   // 0 = untried, 1 = xinput1_4, 2 = xinput9_1_0, -1 = unavailable

        public static bool GetState(int index, out XInputState state)
        {
            state = new XInputState();
            if (dll == -1) return false;
            try
            {
                if (dll != 2) { int r = Get14(index, out state); dll = 1; return r == 0; }
            }
            catch (DllNotFoundException) { dll = 2; }
            catch (EntryPointNotFoundException) { dll = 2; }
            try
            {
                int r = Get910(index, out state);
                dll = 2;
                return r == 0;
            }
            catch (Exception) { dll = -1; return false; }
        }
    }

    // ------------------------------------------------------------------------
    // Settings — %APPDATA%\MILA\controller.ini (same file the C++ controller uses)
    // ------------------------------------------------------------------------

    class Settings
    {
        public string Host = "192.168.4.1";
        public int Port = 5010;
        public string Mode = "wasd";
        public bool HasMode;

        static string PathFor(bool create)
        {
            string dir = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "MILA");
            if (create) { try { Directory.CreateDirectory(dir); } catch (Exception) { } }
            return System.IO.Path.Combine(dir, "controller.ini");
        }

        public static Settings Load()
        {
            Settings s = new Settings();
            try
            {
                foreach (string line in File.ReadAllLines(PathFor(false)))
                {
                    int eq = line.IndexOf('=');
                    if (eq < 0) continue;
                    string k = line.Substring(0, eq).Trim(), v = line.Substring(eq + 1).Trim();
                    int p;
                    if (k == "host" && v.Length > 0) s.Host = v;
                    else if (k == "port" && int.TryParse(v, out p) && p > 0) s.Port = p;
                    else if (k == "mode" && (v == "wasd" || v == "tank" || v == "obstacle")) { s.Mode = v; s.HasMode = true; }
                }
            }
            catch (Exception) { }
            return s;
        }

        public void Save()
        {
            try { File.WriteAllText(PathFor(true), "host=" + Host + "\nport=" + Port + "\nmode=" + Mode + "\n"); }
            catch (Exception) { }
        }
    }

    // ------------------------------------------------------------------------
    // UI — same dark/cyan palette as the onboard dashboard
    // ------------------------------------------------------------------------

    class Btn
    {
        public Rectangle Rect;
        public string Label;
        public Func<string> LabelFn;   // optional dynamic label
        public Func<bool> Vis;         // optional visibility override
        public int Group;              // 0 always, 1 WASD only, 2 TANK only
        public Action Cb;
        public bool Active, Pressed;
    }

    class Series
    {
        public List<double> V = new List<double>();
        public Color Col;
    }

    class MainForm : Form
    {
        static readonly Color BG = Color.FromArgb(10, 10, 15);
        static readonly Color PANEL = Color.FromArgb(18, 18, 26);
        static readonly Color BORDER = Color.FromArgb(30, 30, 46);
        static readonly Color ACCENT = Color.FromArgb(0, 212, 255);
        static readonly Color RED = Color.FromArgb(255, 68, 68);
        static readonly Color AMBER = Color.FromArgb(255, 190, 60);
        static readonly Color GREEN = Color.FromArgb(0, 255, 136);
        static readonly Color TEXT = Color.FromArgb(224, 224, 240);
        static readonly Color DIM = Color.FromArgb(102, 102, 136);
        static readonly Color ACCENT_BG = Color.FromArgb(14, 34, 46);

        const int W = 840, H = 720;     // left 420px = controls, right 420px = radar / graphs / connection
        const int SL_L = 80, SL_R = 340, SL_Y = 172;   // speed slider

        readonly WifiLink link;
        readonly Settings settings;
        string host;
        int port;
        readonly List<Btn> btns = new List<Btn>();
        readonly Btn[] modeBtns = new Btn[3];
        readonly HashSet<Keys> down = new HashSet<Keys>();
        readonly Font fBig, fMed, fSml;
        string mode;                    // mirrors the firmware's WASD boot default
        bool modeSynced;
        string activeDrive = null;
        int speedPct = 100;
        bool sliderDrag, speedPending;
        int speedHold, lastSpeedSend;
        string exportMsg = "";

        // discovery
        readonly object scanLk = new object();
        readonly List<string> found = new List<string>();
        volatile bool scanning;
        bool scanEver;
        int scanDone, scanTotal;
        volatile string autoIp;
        bool autoConnect;

        // gamepad
        int padIndex = -1;
        int padNextProbe, padLastTick;
        ushort padPrevButtons;
        string padDrive = "";
        int padL, padR;
        double padSpeedF = 100;

        public MainForm(Settings settings, bool autoHost)
        {
            this.settings = settings;
            host = settings.Host;
            port = settings.Port;
            mode = settings.Mode;
            link = new WifiLink(host, port);

            Text = "MILA Control";
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            ClientSize = new Size(W, H);
            StartPosition = FormStartPosition.CenterScreen;
            BackColor = BG;
            KeyPreview = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);

            fBig = new Font("Consolas", 26f, FontStyle.Bold, GraphicsUnit.Pixel);
            fMed = new Font("Consolas", 15f, FontStyle.Bold, GraphicsUnit.Pixel);
            fSml = new Font("Consolas", 12f, FontStyle.Regular, GraphicsUnit.Pixel);

            BuildButtons();

            System.Windows.Forms.Timer t = new System.Windows.Forms.Timer();
            t.Interval = 16;
            t.Tick += delegate { OnTick(); };
            t.Start();

            if (autoHost) StartScan(true);
        }

        // ---- actions ----

        void StartDrive(string action) { activeDrive = action; link.Cmd(action); }

        void StopDrive()
        {
            if (activeDrive != null) { activeDrive = null; link.Cmd("STOP"); }
        }

        void ApplyModeButtons()
        {
            string[] names = { "wasd", "tank", "obstacle" };
            for (int i = 0; i < 3; i++) modeBtns[i].Active = (names[i] == mode);
        }

        void SetMode(string m)
        {
            mode = m;
            link.Mode(m);
            ApplyModeButtons();
            SaveSettings();
        }

        void CycleMode(int dir)
        {
            string[] names = { "wasd", "tank", "obstacle" };
            int cur = Math.Max(0, Array.IndexOf(names, mode));
            SetMode(names[(cur + dir + 3) % 3]);
        }

        void SaveSettings()
        {
            settings.Host = host;
            settings.Port = port;
            settings.Mode = mode;
            settings.Save();
        }

        // Local speed change: shown immediately, sent (throttled) by FlushSpeed().
        void SetSpeedLocal(int v)
        {
            v = Math.Max(25, Math.Min(100, v));
            if (v == speedPct && !speedPending) return;
            speedPct = v;
            speedPending = true;
            speedHold = Environment.TickCount + 1500;   // don't let stale telemetry yank the slider back
        }

        void FlushSpeed()
        {
            int now = Environment.TickCount;
            if (speedPending && now - lastSpeedSend >= 80)
            {
                speedPending = false;
                lastSpeedSend = now;
                link.Cmd("SPEED:" + speedPct);
            }
        }

        void ConnectTo(string newHost)
        {
            host = newHost;
            link.Retarget(host, port);
            modeSynced = false;
            SaveSettings();
        }

        // ---- discovery ----

        static List<string> LocalSubnets()
        {
            List<string> outp = new List<string>();
            try
            {
                foreach (NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                    foreach (UnicastIPAddressInformation ua in ni.GetIPProperties().UnicastAddresses)
                    {
                        if (ua.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                        byte[] b = ua.Address.GetAddressBytes();
                        if (b[0] == 127 || (b[0] == 169 && b[1] == 254)) continue;
                        string pre = b[0] + "." + b[1] + "." + b[2] + ".";
                        if (!outp.Contains(pre)) outp.Add(pre);
                    }
                }
            }
            catch (Exception) { }
            return outp;
        }

        void StartScan(bool auto)
        {
            if (scanning) return;
            scanning = true;
            scanEver = true;
            scanDone = 0;
            scanTotal = 0;
            autoConnect = auto;
            autoIp = null;
            lock (scanLk) { found.Clear(); }
            int scanPort = port;

            Thread t = new Thread(delegate ()
            {
                List<string> cands = new List<string>();
                HashSet<string> seen = new HashSet<string>();
                Action<string> add = delegate (string s) { if (seen.Add(s)) cands.Add(s); };

                // Robots registered with NORA (best effort: pull every IPv4 out of the reply).
                string reg = WifiLink.HttpGetString("http://192.168.4.1:5000/robots", 400);
                if (reg != null)
                    foreach (Match m in Regex.Matches(reg, @"(\d{1,3}\.){3}\d{1,3}")) add(m.Value);
                add("127.0.0.1");    // a simulator running on this machine (sim/MockMila.cs)
                add("mila.local");   // the firmware advertises itself over mDNS
                add("192.168.4.1");  // MILA's own access point
                List<string> prefixes = LocalSubnets();
                prefixes.Add("192.168.4.");
                foreach (string pre in prefixes)
                    for (int i = 1; i <= 254; i++) add(pre + i);

                scanTotal = cands.Count;
                int next = -1;
                ThreadStart worker = delegate ()
                {
                    for (;;)
                    {
                        int i = Interlocked.Increment(ref next);
                        if (i >= cands.Count) break;
                        if (WifiLink.Probe(cands[i], scanPort))
                        {
                            lock (scanLk) { found.Add(cands[i]); }
                            if (autoConnect && autoIp == null) autoIp = cands[i];
                        }
                        Interlocked.Increment(ref scanDone);
                    }
                };
                List<Thread> pool = new List<Thread>();
                for (int i = 0; i < 64; i++) { Thread w = new Thread(worker); w.IsBackground = true; w.Start(); pool.Add(w); }
                foreach (Thread w in pool) w.Join();
                scanning = false;
            });
            t.IsBackground = true;
            t.Start();
        }

        string ScanLabel()
        {
            if (!scanning) return "SCAN NETWORK";
            int total = scanTotal;
            return "SCANNING " + (total > 0 ? 100 * scanDone / total : 0) + "%";
        }

        // ---- CSV export ----

        void ExportCsv()
        {
            List<Sample> samples = link.All();
            if (samples.Count == 0) { exportMsg = "no samples yet"; return; }
            try
            {
                string dir = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "MILA-telemetry");
                Directory.CreateDirectory(dir);
                string fname = "telemetry_" + DateTime.Now.ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture) + ".csv";
                StringBuilder sb = new StringBuilder();
                sb.Append("timestamp,dist_cm,left_cm,right_cm,temp_c,hum_pct,speed_pct,mode,last_cmd\n");
                foreach (Sample s in samples)
                {
                    sb.Append(s.Time.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture)).Append(',')
                      .Append(Csv(s.Dist)).Append(',').Append(Csv(s.Left)).Append(',').Append(Csv(s.Right)).Append(',')
                      .Append(Csv(s.Temp)).Append(',').Append(Csv(s.Hum)).Append(',').Append(Csv(s.Speed)).Append(',')
                      .Append(s.Mode).Append(',').Append(s.Cmd).Append('\n');
                }
                File.WriteAllText(System.IO.Path.Combine(dir, fname), sb.ToString());
                exportMsg = "saved " + samples.Count + " rows: " + fname;
            }
            catch (Exception) { exportMsg = "could not write file"; }
        }

        static string Csv(double v)
        {
            return double.IsNaN(v) ? "" : v.ToString("R", CultureInfo.InvariantCulture);
        }

        // ---- gamepad ----

        void PollPad()
        {
            int now = Environment.TickCount;
            XInputState st;
            if (padIndex < 0)
            {
                if (now - padNextProbe < 0) return;
                padNextProbe = now + 1000;   // probing empty slots is slow, so only once a second
                for (int i = 0; i < 4; i++)
                {
                    if (XInput.GetState(i, out st)) { padIndex = i; padLastTick = now; break; }
                }
                if (padIndex < 0) return;
            }
            if (!XInput.GetState(padIndex, out st))
            {
                // Unplugged mid-drive: stop the robot.
                padIndex = -1;
                padDrive = "";
                padL = padR = 0;
                StopDrive();
                return;
            }

            XInputGamepad p = st.Gamepad;
            const double dz = 0.45;
            double lx = Math.Max(-1.0, p.ThumbLX / 32767.0);
            double ly = Math.Max(-1.0, p.ThumbLY / 32767.0);
            double ry = Math.Max(-1.0, p.ThumbRY / 32767.0);
            double dt = (now - padLastTick) / 1000.0;
            padLastTick = now;

            // A = stop, LB/RB = previous/next mode (edge-triggered).
            ushort pressed = (ushort)(p.Buttons & ~padPrevButtons);
            padPrevButtons = p.Buttons;
            if ((pressed & XInput.A) != 0)
            {
                padDrive = "";
                padL = padR = 0;
                activeDrive = null;
                link.Cmd("STOP");
            }
            if ((pressed & XInput.LB) != 0) CycleMode(-1);
            if ((pressed & XInput.RB) != 0) CycleMode(+1);

            if (mode == "wasd")
            {
                string want = "";
                if (Math.Sqrt(lx * lx + ly * ly) >= dz)
                {
                    if (Math.Abs(ly) >= Math.Abs(lx)) want = ly > 0 ? "FORWARD" : "BACKWARD";
                    else want = lx > 0 ? "RIGHT" : "LEFT";
                }
                else if ((p.Buttons & XInput.DPAD_UP) != 0) want = "FORWARD";
                else if ((p.Buttons & XInput.DPAD_DOWN) != 0) want = "BACKWARD";
                else if ((p.Buttons & XInput.DPAD_LEFT) != 0) want = "LEFT";
                else if ((p.Buttons & XInput.DPAD_RIGHT) != 0) want = "RIGHT";
                if (want != padDrive)
                {
                    padDrive = want;
                    if (want.Length == 0) StopDrive();
                    else StartDrive(want);
                }
            }
            else if (mode == "tank")
            {
                // Left stick Y = left track, right stick Y = right track.
                int l = ly >= dz ? 1 : (ly <= -dz ? -1 : 0);
                int r = ry >= dz ? 1 : (ry <= -dz ? -1 : 0);
                if (l != padL || r != padR)
                {
                    if (l == 0 && r == 0)
                    {
                        activeDrive = null;
                        link.Cmd("STOP");
                    }
                    else
                    {
                        // STOP is global, so when a track lets go, stop and re-issue the other.
                        bool released = (padL != 0 && l == 0) || (padR != 0 && r == 0);
                        if (released) link.Cmd("STOP");
                        if (l != 0 && (released || l != padL)) link.Cmd(l > 0 ? "L_FWD" : "L_BWD");
                        if (r != 0 && (released || r != padR)) link.Cmd(r > 0 ? "R_FWD" : "R_BWD");
                        activeDrive = "PAD_TANK";   // sentinel so Space / focus loss still sends STOP
                    }
                    padL = l;
                    padR = r;
                }
            }
            else
            {
                padDrive = "";
                padL = padR = 0;
            }

            // Triggers ramp speed: RT up, LT down (full pull = 60 %/s).
            double rt = p.RightTrigger / 255.0, lt = p.LeftTrigger / 255.0;
            if (rt > 0.1 || lt > 0.1)
            {
                padSpeedF = Math.Max(25.0, Math.Min(100.0, padSpeedF + (rt - lt) * 60.0 * dt));
                SetSpeedLocal((int)Math.Round(padSpeedF));
            }
            else padSpeedF = speedPct;
        }

        void OnTick()
        {
            PollPad();
            FlushSpeed();

            string ip = autoIp;
            if (ip != null) { autoIp = null; ConnectTo(ip); }

            // Once connected: push a remembered mode to the robot, or adopt the robot's current one.
            if (!modeSynced && link.Ok)
            {
                Dictionary<string, string> t = link.Telemetry();
                string rm;
                bool has = t.TryGetValue("mode", out rm);
                if (settings.HasMode)
                {
                    if (!has || rm != mode) link.Mode(mode);
                }
                else if (has && (rm == "wasd" || rm == "tank" || rm == "obstacle"))
                {
                    mode = rm;
                    ApplyModeButtons();
                }
                modeSynced = true;
            }
            Invalidate();
        }

        // ---- layout ----

        Btn Add(int x, int y, int w, int h, string label, Action cb, int group, bool active)
        {
            Btn b = new Btn();
            b.Rect = new Rectangle(x, y, w, h);
            b.Label = label;
            b.Cb = cb;
            b.Group = group;
            b.Active = active;
            btns.Add(b);
            return b;
        }

        void BuildButtons()
        {
            modeBtns[0] = Add(20, 100, 120, 40, "WASD", delegate { SetMode("wasd"); }, 0, false);
            modeBtns[1] = Add(150, 100, 120, 40, "TANK", delegate { SetMode("tank"); }, 0, false);
            modeBtns[2] = Add(280, 100, 120, 40, "OBSTACLE", delegate { SetMode("obstacle"); }, 0, false);
            ApplyModeButtons();

            // Speed: [-]  slider  [+]. Presets mirror the firmware's speedPresets[] (100/75/50/25).
            Add(20, 150, 44, 40, "-", delegate { SetSpeedLocal(speedPct - 25); }, 0, false);
            Add(356, 150, 44, 40, "+", delegate { SetSpeedLocal(speedPct + 25); }, 0, false);

            // WASD d-pad
            int cx = 210, cy = 280, bs = 70, gap = 8;
            Add(cx - bs / 2, cy - bs - gap, bs, bs, "^", delegate { StartDrive("FORWARD"); }, 1, false);
            Add(cx - bs / 2, cy + gap, bs, bs, "v", delegate { StartDrive("BACKWARD"); }, 1, false);
            Add(cx - bs - gap - bs / 2 - gap, cy - bs / 2, bs, bs, "<", delegate { StartDrive("LEFT"); }, 1, false);
            Add(cx + gap + bs / 2 + gap, cy - bs / 2, bs, bs, ">", delegate { StartDrive("RIGHT"); }, 1, false);
            Add(cx - bs / 2, cy - bs / 2, bs, bs, "STOP", delegate { link.Cmd("STOP"); }, 1, false);

            // TANK track buttons
            int ty = 280;
            Add(60, ty - 40, 90, 50, "Q\nL-FWD", delegate { StartDrive("L_FWD"); }, 2, false);
            Add(60, ty + 40, 90, 50, "A\nL-BWD", delegate { StartDrive("L_BWD"); }, 2, false);
            Add(420 - 150, ty - 40, 90, 50, "E\nR-FWD", delegate { StartDrive("R_FWD"); }, 2, false);
            Add(420 - 150, ty + 40, 90, 50, "D\nR-BWD", delegate { StartDrive("R_BWD"); }, 2, false);

            // Right column: CSV export, robot discovery.
            Add(440, 545, 180, 36, "EXPORT CSV", delegate { ExportCsv(); }, 0, false);

            Btn scan = Add(440, 622, 150, 34, "SCAN", delegate { StartScan(false); }, 0, false);
            scan.LabelFn = delegate { return ScanLabel(); };
            for (int i = 0; i < 3; i++)
            {
                int idx = i;
                Btn s = Add(440 + i * 130, 668, 122, 34, "", delegate
                {
                    string ip = null;
                    lock (scanLk) { if (idx < found.Count) ip = found[idx]; }
                    if (ip != null) ConnectTo(ip);
                }, 0, false);
                s.LabelFn = delegate { lock (scanLk) { return idx < found.Count ? found[idx] : ""; } };
                s.Vis = delegate { lock (scanLk) { return idx < found.Count; } };
            }
        }

        bool IsShown(Btn b)
        {
            if (b.Vis != null) return b.Vis();
            return b.Group == 0 || (b.Group == 1 && mode == "wasd") || (b.Group == 2 && mode == "tank");
        }

        string KeyDrive(Keys k)
        {
            if (mode == "wasd")
            {
                switch (k)
                {
                    case Keys.Up: case Keys.W: return "FORWARD";
                    case Keys.Down: case Keys.S: return "BACKWARD";
                    case Keys.Left: case Keys.A: return "LEFT";
                    case Keys.Right: case Keys.D: return "RIGHT";
                }
            }
            else if (mode == "tank")
            {
                switch (k)
                {
                    case Keys.Q: return "L_FWD";
                    case Keys.A: return "L_BWD";
                    case Keys.E: return "R_FWD";
                    case Keys.D: return "R_BWD";
                }
            }
            return null;
        }

        // ---- input ----

        protected override bool IsInputKey(Keys keyData)
        {
            return true;    // deliver arrow keys to OnKeyDown
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            e.Handled = true;
            if (!down.Add(e.KeyCode)) return;   // ignore OS auto-repeat

            if (e.KeyCode == Keys.Escape) Close();
            else if (e.KeyCode == Keys.D1) SetMode("wasd");
            else if (e.KeyCode == Keys.D2) SetMode("tank");
            else if (e.KeyCode == Keys.D3) SetMode("obstacle");
            else if (e.KeyCode == Keys.Space) StopDrive();
            else
            {
                string a = KeyDrive(e.KeyCode);
                if (a != null) StartDrive(a);
            }
        }

        protected override void OnKeyUp(KeyEventArgs e)
        {
            e.Handled = true;
            down.Remove(e.KeyCode);
            string a = KeyDrive(e.KeyCode);
            if (a != null && a == activeDrive) StopDrive();
        }

        void SetSliderFromX(int x)
        {
            SetSpeedLocal(25 + (int)Math.Round((x - SL_L) * 75.0 / (SL_R - SL_L)));
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button != MouseButtons.Left) return;
            if (new Rectangle(SL_L - 14, SL_Y - 16, SL_R - SL_L + 28, 32).Contains(e.Location))
            {
                sliderDrag = true;
                SetSliderFromX(e.X);
                return;
            }
            foreach (Btn b in btns)
            {
                if (IsShown(b) && b.Rect.Contains(e.Location))
                {
                    b.Pressed = true;
                    b.Cb();
                    break;
                }
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (sliderDrag) SetSliderFromX(e.X);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            sliderDrag = false;
            ReleaseAll();
        }

        // Don't leave the robot driving if the window loses focus mid-press.
        protected override void OnDeactivate(EventArgs e)
        {
            base.OnDeactivate(e);
            down.Clear();
            sliderDrag = false;
            ReleaseAll();
        }

        void ReleaseAll()
        {
            foreach (Btn b in btns) b.Pressed = false;
            StopDrive();
        }

        protected override void OnFormClosed(FormClosedEventArgs e)
        {
            base.OnFormClosed(e);
            SaveSettings();
            link.StopNow();
            link.Dispose();
        }

        // ---- drawing ----

        static GraphicsPath RoundRect(Rectangle r, int radius)
        {
            int d = radius * 2;
            GraphicsPath p = new GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }

        static void Panel(Graphics g, Rectangle r, Color fill, Color line, float lineW, int radius)
        {
            using (GraphicsPath p = RoundRect(r, radius))
            using (SolidBrush br = new SolidBrush(fill))
            using (Pen pen = new Pen(line, lineW))
            {
                g.FillPath(br, p);
                g.DrawPath(pen, p);
            }
        }

        static void Str(Graphics g, string s, Font f, Color c, float x, float y, StringAlignment h, StringAlignment v)
        {
            using (SolidBrush br = new SolidBrush(c))
            using (StringFormat sf = new StringFormat(StringFormat.GenericTypographic))
            {
                sf.Alignment = h;
                sf.LineAlignment = v;
                g.DrawString(s, f, br, x, y, sf);
            }
        }

        static string Get(Dictionary<string, string> t, string k)
        {
            string v;
            return t.TryGetValue(k, out v) ? v : "--";
        }

        static double Nv(Dictionary<string, string> t, string k)
        {
            string v;
            double d;
            return t.TryGetValue(k, out v) && double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out d) ? d : double.NaN;
        }

        static string Fmt(double v, string fmt)
        {
            return double.IsNaN(v) ? "--" : v.ToString(fmt, CultureInfo.InvariantCulture);
        }

        static Color Mix(Color a, Color b, double t)
        {
            return Color.FromArgb((int)Math.Round(a.R * t + b.R * (1 - t)), (int)Math.Round(a.G * t + b.G * (1 - t)),
                                  (int)Math.Round(a.B * t + b.B * (1 - t)));
        }

        static Color ProxColor(double cm) { return cm < 20 ? RED : (cm < 50 ? AMBER : GREEN); }

        void Tile(Graphics g, int x, int y, int w, string label, string value)
        {
            Panel(g, new Rectangle(x, y, w, 46), PANEL, BORDER, 1f, 6);
            Str(g, label, fSml, DIM, x + 8, y + 6, StringAlignment.Near, StringAlignment.Near);
            Str(g, value, fMed, ACCENT, x + 8, y + 22, StringAlignment.Near, StringAlignment.Near);
        }

        void DrawButton(Graphics g, Btn b)
        {
            bool on = b.Active || b.Pressed;
            Panel(g, b.Rect, on ? ACCENT_BG : PANEL, on ? ACCENT : BORDER, 2f, 8);
            string label = b.LabelFn != null ? b.LabelFn() : b.Label;
            Str(g, label, fMed, on ? ACCENT : TEXT, b.Rect.X + b.Rect.Width / 2f, b.Rect.Y + b.Rect.Height / 2f,
                StringAlignment.Center, StringAlignment.Center);
        }

        // Radar: three wedges (left / front / right) whose length is the distance, coloured by proximity.
        void DrawRadar(Graphics g, Rectangle panel, double left, double front, double right, bool live)
        {
            Panel(g, panel, PANEL, BORDER, 1f, 8);
            Str(g, "PROXIMITY", fSml, DIM, panel.Left + 12, panel.Top + 8, StringAlignment.Near, StringAlignment.Near);

            const double maxRange = 100.0;
            int cx = (panel.Left + panel.Right) / 2, cy = panel.Bottom - 36, R = 170;

            using (Pen ring = new Pen(BORDER, 1f))
            {
                for (int i = 1; i <= 4; i++)
                {
                    int r = R * i / 4;
                    g.DrawArc(ring, cx - r, cy - r, 2 * r, 2 * r, 180, 180);   // top half
                }
                g.DrawLine(ring, cx - R, cy, cx + R, cy);
            }

            // GDI+ angles run clockwise from +x, ours run counter-clockwise: start = -a2, sweep = a2 - a1.
            double[][] spans = { new double[] { 125, 165 }, new double[] { 70, 110 }, new double[] { 15, 55 } };
            double[] vals = { left, front, right };
            string[] names = { "LEFT", "FRONT", "RIGHT" };
            float[] lx = { panel.Left + 80, cx, panel.Right - 80 };
            for (int i = 0; i < 3; i++)
            {
                double v = vals[i];
                bool known = !double.IsNaN(v);
                Color col = (!known || !live) ? DIM : ProxColor(v);
                if (known)
                {
                    int r = (int)Math.Round(R * Math.Max(0.06, Math.Min(1.0, v / maxRange)));
                    using (SolidBrush br = new SolidBrush(Mix(col, PANEL, 0.35)))
                    using (Pen pen = new Pen(col, 2f))
                    {
                        g.FillPie(br, cx - r, cy - r, 2 * r, 2 * r, (float)-spans[i][1], (float)(spans[i][1] - spans[i][0]));
                        g.DrawPie(pen, cx - r, cy - r, 2 * r, 2 * r, (float)-spans[i][1], (float)(spans[i][1] - spans[i][0]));
                    }
                }
                Str(g, names[i], fSml, DIM, lx[i], cy + 6, StringAlignment.Center, StringAlignment.Near);
                Str(g, known ? Fmt(v, "0") + " cm" : "--", fMed, col, lx[i], cy + 18, StringAlignment.Center, StringAlignment.Near);
            }
            using (SolidBrush tb = new SolidBrush(ACCENT))
                g.FillPolygon(tb, new Point[] { new Point(cx, cy - 10), new Point(cx - 7, cy + 2), new Point(cx + 7, cy + 2) });
        }

        // Line chart over the last 150 samples. Each series is scaled to lo..hi (lo == hi means auto).
        void DrawGraph(Graphics g, Rectangle box, string title, List<Series> series, double lo, double hi, string[] latest)
        {
            Panel(g, box, PANEL, BORDER, 1f, 8);
            Str(g, title, fSml, DIM, box.Left + 12, box.Top + 7, StringAlignment.Near, StringAlignment.Near);
            float rx = box.Right - 12;
            for (int i = series.Count - 1; i >= 0; i--)
            {
                SizeF sz = g.MeasureString(latest[i], fMed, 1000, StringFormat.GenericTypographic);
                Str(g, latest[i], fMed, series[i].Col, rx, box.Top + 5, StringAlignment.Far, StringAlignment.Near);
                rx -= sz.Width + 14;
            }

            Rectangle plot = new Rectangle(box.Left + 12, box.Top + 28, box.Width - 24, box.Height - 38);
            using (Pen grid = new Pen(BORDER, 1f))
                for (int i = 0; i <= 2; i++)
                {
                    int y = plot.Top + plot.Height * i / 2;
                    g.DrawLine(grid, plot.Left, y, plot.Right, y);
                }

            foreach (Series s in series)
            {
                double mn = lo, mx = hi;
                if (mn == mx)   // auto-scale this series with a little headroom
                {
                    mn = double.MaxValue;
                    mx = double.MinValue;
                    foreach (double v in s.V)
                        if (!double.IsNaN(v)) { mn = Math.Min(mn, v); mx = Math.Max(mx, v); }
                    if (mn > mx) continue;
                    if (mx - mn < 4) { double mid = (mx + mn) / 2; mn = mid - 2; mx = mid + 2; }
                }
                using (Pen pen = new Pen(s.Col, 2f))
                {
                    List<PointF> run = new List<PointF>();
                    for (int i = 0; i <= s.V.Count; i++)
                    {
                        if (i == s.V.Count || double.IsNaN(s.V[i]))
                        {
                            if (run.Count > 1) g.DrawLines(pen, run.ToArray());
                            run.Clear();
                            continue;
                        }
                        float x = plot.Left + plot.Width * (float)i / 149f;
                        double f = Math.Max(0.0, Math.Min(1.0, (s.V[i] - mn) / (mx - mn)));
                        run.Add(new PointF(x, plot.Bottom - (float)(plot.Height * f)));
                    }
                }
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
            g.Clear(BG);

            Dictionary<string, string> t = link.Telemetry();
            bool ok = link.Ok;
            double spv;
            string sp;
            if (t.TryGetValue("speed", out sp) && !sliderDrag && Environment.TickCount - speedHold > 0 && !speedPending &&
                double.TryParse(sp, NumberStyles.Float, CultureInfo.InvariantCulture, out spv))
                speedPct = (int)spv;

            // ---- left column: controls ----
            Str(g, "MILA", fBig, ACCENT, 210, 28, StringAlignment.Center, StringAlignment.Center);
            string sub = host + ":" + port + "   " + (ok ? "connected" : "OFFLINE") + (padIndex >= 0 ? "   [gamepad]" : "");
            Str(g, sub, fSml, ok ? GREEN : RED, 210, 54, StringAlignment.Center, StringAlignment.Center);

            foreach (Btn b in btns)
                if (IsShown(b) && b.Rect.Left < 420) DrawButton(g, b);

            // speed slider
            using (SolidBrush track = new SolidBrush(BORDER))
                g.FillRectangle(track, SL_L, SL_Y - 3, SL_R - SL_L, 6);
            int kx = SL_L + (speedPct - 25) * (SL_R - SL_L) / 75;
            using (SolidBrush fill = new SolidBrush(ACCENT))
                g.FillRectangle(fill, SL_L, SL_Y - 3, kx - SL_L, 6);
            using (SolidBrush kb = new SolidBrush(sliderDrag ? ACCENT : TEXT))
            using (Pen kp = new Pen(ACCENT, 2f))
            {
                g.FillEllipse(kb, kx - 10, SL_Y - 10, 20, 20);
                g.DrawEllipse(kp, kx - 10, SL_Y - 10, 20, 20);
            }
            Str(g, "SPEED " + speedPct + "%", fSml, DIM, (SL_L + SL_R) / 2f, 156, StringAlignment.Center, StringAlignment.Center);

            int sy = 400, colw = (420 - 50) / 2, c2 = 30 + colw;
            Tile(g, 20, sy, colw, "FRONT CM", Get(t, "dist"));
            Tile(g, c2, sy, colw, "LEFT CM", Get(t, "left"));
            Tile(g, 20, sy + 54, colw, "RIGHT CM", Get(t, "right"));
            Tile(g, c2, sy + 54, colw, "LAST CMD", Get(t, "cmd"));
            Tile(g, 20, sy + 108, colw, "LAST IR", Get(t, "ir"));
            Tile(g, c2, sy + 108, colw, "TEMP C", Get(t, "temp"));
            Tile(g, 20, sy + 162, colw, "HUMIDITY %", Get(t, "hum"));
            Tile(g, c2, sy + 162, colw, "IP  " + Get(t, "ip"),
                 Get(t, "fleet") == "1" ? "FLEET (joined NORA)" : "STANDALONE AP");
            Tile(g, 20, sy + 216, colw, "SPEED %", Get(t, "speed"));
            Tile(g, c2, sy + 216, colw, "GUARD", Get(t, "guard") == "1" ? "ACTIVE" : "clear");

            Str(g, "1/2/3 mode - WASD/QAED drive - space stop - esc quit", fSml, DIM,
                210, H - 18, StringAlignment.Center, StringAlignment.Center);

            // ---- right column: radar, graphs, export, discovery ----
            DrawRadar(g, new Rectangle(440, 20, 380, 242), Nv(t, "left"), Nv(t, "dist"), Nv(t, "right"), ok);

            List<Sample> hist = link.Tail(150);
            Series sDist = new Series(), sTemp = new Series(), sHum = new Series();
            sDist.Col = ACCENT; sTemp.Col = ACCENT; sHum.Col = GREEN;
            double dmax = 100;
            foreach (Sample s in hist)
            {
                sDist.V.Add(s.Dist);
                sTemp.V.Add(s.Temp);
                sHum.V.Add(s.Hum);
                if (!double.IsNaN(s.Dist)) dmax = Math.Max(dmax, s.Dist);
            }
            double lastDist = hist.Count > 0 ? hist[hist.Count - 1].Dist : double.NaN;
            double lastTemp = hist.Count > 0 ? hist[hist.Count - 1].Temp : double.NaN;
            double lastHum = hist.Count > 0 ? hist[hist.Count - 1].Hum : double.NaN;
            DrawGraph(g, new Rectangle(440, 272, 380, 120), "FRONT DISTANCE (LAST ~60 S)",
                      new List<Series> { sDist }, 0, dmax, new string[] { Fmt(lastDist, "0.0") + " cm" });
            DrawGraph(g, new Rectangle(440, 402, 380, 120), "TEMP / HUMIDITY",
                      new List<Series> { sTemp, sHum }, 0, 0,
                      new string[] { Fmt(lastTemp, "0.0") + " C", Fmt(lastHum, "0.0") + " %" });

            foreach (Btn b in btns)
                if (IsShown(b) && b.Rect.Left >= 420) DrawButton(g, b);
            Str(g, exportMsg, fSml, DIM, 632, 556, StringAlignment.Near, StringAlignment.Near);

            string status;
            lock (scanLk)
            {
                if (scanning) status = "looking for MILA on your network...";
                else if (scanEver) status = found.Count == 0 ? "no robots found" : "click an address to connect";
                else status = "scan to find MILA (or use --host auto)";
            }
            Str(g, "ROBOT  " + host + ":" + port, fSml, DIM, 440, 600, StringAlignment.Near, StringAlignment.Near);
            Str(g, status, fSml, DIM, 600, 632, StringAlignment.Near, StringAlignment.Near);
        }
    }

    static class Program
    {
        [STAThread]
        static void Main(string[] args)
        {
            Settings settings = Settings.Load();
            bool autoHost = false;
            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--host" && i + 1 < args.Length)
                {
                    string h = args[++i];
                    if (h == "auto") autoHost = true;
                    else settings.Host = h;
                }
                else if (args[i] == "--port" && i + 1 < args.Length)
                {
                    int p;
                    if (int.TryParse(args[++i], out p)) settings.Port = p;
                }
            }
            Application.EnableVisualStyles();
            Application.Run(new MainForm(settings, autoHost));
        }
    }
}
